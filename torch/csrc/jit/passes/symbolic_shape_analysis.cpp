#include <ATen/core/interned_strings.h>
#include <c10/util/Exception.h>
#include <torch/csrc/jit/ir/alias_analysis.h>
#include <torch/csrc/jit/ir/constants.h>
#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/passes/common_subexpression_elimination.h>
#include <torch/csrc/jit/passes/constant_pooling.h>
#include <torch/csrc/jit/passes/constant_propagation.h>
#include <torch/csrc/jit/passes/dead_code_elimination.h>
#include <torch/csrc/jit/passes/loop_unrolling.h>
#include <torch/csrc/jit/passes/lower_tuples.h>
#include <torch/csrc/jit/passes/peephole.h>
#include <torch/csrc/jit/passes/remove_mutation.h>
#include <torch/csrc/jit/passes/symbolic_shape_analysis.h>
#include <torch/csrc/jit/runtime/exception_message.h>
#include <torch/csrc/utils/memory.h>
#include <memory>
#include <unordered_map>
#include <vector>

namespace torch {
namespace jit {

// TODO: better registration mechanism
std::mutex lock;
std::unordered_map<std::string, std::shared_ptr<Graph>> operator_functions;

c10::optional<size_t> normIndex(int64_t index, size_t len) {
  if (index < 0) {
    index = index + len;
  }
  if (index >= 0 && index < static_cast<int64_t>(len)) {
    return index;
  } else {
    return c10::nullopt;
  }
}

void replaceWithIValue(Value* v, IValue val) {
  WithInsertPoint guard(*v->node()->owningBlock()->nodes().begin());
  v->replaceAllUsesWith(v->owningGraph()->insertConstant(val));
}

// Symbolic Shape Analysis works through iteratively partially evaluating
// a TorchScript shape compute graph by inputting properties from input
// Tensors. We can substitute in properties like `len(x)` and `x[1]`
// if they are statically on the input Tensors. We can also use
// assertions like `assert len(x) == 4` in order to refine the input
// length and unroll loops over its elements. We iteratively optimize and
// substitute in properties until we are unable to make any further
// optimizations. Finally, we try to extract Tensor properties from the output.
// For instance `return [1, 2, inp[2] + 1, inp[3]]` we know that the ouptut
// will be length 4 with first two dimensions equal to 1 and 2.
// It is not implemented yet but in the future we will also be able to
// infer that the 4th dimension will have the same symbolic shape as inp[3]

struct SymbolicShapeAnalyzer {
  SymbolicShapeAnalyzer(Node* n, std::shared_ptr<Graph> shape_compute_graph)
      : graph_(shape_compute_graph->copy()), node_(n) {
    for (size_t i = 0; i < node_->inputs().size(); i++) {
      auto type = node_->input(i)->type();
      if (auto tt = type->castRaw<TensorType>()) {
        c10::SymbolicShape symbolic_shapes = tt->symbolic_sizes();
        if (symbolic_shapes.isComplete()) {
          replaceWithIValue(
              graph_->inputs().at(i), *tt->sizes().concrete_sizes());
          continue;
        }
        // we can
        if (symbolic_shapes.rank()) {
          node_input_tensor_indices.push_back(i);
        }
      } else if (
          type->cast<ListType>() &&
          type->cast<ListType>()->getElementType()->cast<TensorType>()) {
        TORCH_INTERNAL_ASSERT(false); // not handled yet
      } else {
        if (auto ival = toIValue(node_->input(i))) {
          replaceWithIValue(graph_->inputs().at(i), *ival);
        }
      }
    }
  }

  c10::SymbolicShape run() {
    // TODO: only run while the last iteration has made a change
    size_t num_optimization_iters = 6;
    for (size_t i = 0; i < num_optimization_iters; i++) {
      substituteInputTensorProperties();
      LowerSimpleTuples(graph_);
      RemoveListMutation(graph_);
      UnrollConstantLoops(graph_);
      ConstantPropagation(graph_);
      PeepholeOptimize(graph_);
      ConstantPropagation(graph_);
    }
    ConstantPooling(graph_);
    EliminateDeadCode(graph_);
    return extractOutputShape();
  }

 private:
  void substituteInputTensorProperties() {
    for (auto index : node_input_tensor_indices) {
      substituteTensorProperties(index);
    }
  }

  void substituteTensorProperties(int64_t node_input_tensor_index) {
    auto node_tensor_index_value = node_->input(node_input_tensor_index);
    auto shape =
        node_tensor_index_value->type()->expect<TensorType>()->symbolic_sizes();
    if (!shape.rank().has_value()) {
      return;
    }

    for (const auto& use :
         graph_->inputs().at(node_input_tensor_index)->uses()) {
      // TODO: either decompose composite ops like slice or add handling here
      switch (use.user->kind()) {
        case aten::len: {
          size_t len = shape.rank().value();
          replaceWithIValue(use.user->output(), static_cast<int64_t>(len));
        } break;
        case aten::__getitem__: {
          auto index = constant_as<int64_t>(use.user->inputs().at(1));
          if (index) {
            auto norm_index = normIndex(*index, *shape.rank());
            // TODO: HANDLE non-static value (symbolic shape)
            if (norm_index && shape[*norm_index].is_static()) {
              replaceWithIValue(
                  use.user->output(), shape[*norm_index].static_size());
            }
          }
        }
      }
    }
  }

  c10::SymbolicShape extractOutputShape() {
    TORCH_INTERNAL_ASSERT(graph_->outputs().size() == 1);
    auto output = graph_->outputs().at(0);
    TORCH_INTERNAL_ASSERT(
        output->type()->cast<ListType>() &&
        output->type()->cast<ListType>()->getElementType()->cast<IntType>());
    if (output->node()->kind() == prim::Constant) {
      auto int_list = toIValue(output)->toIntVector();
      return c10::SymbolicShape(int_list);
    }
    // If it is not a single list construct or constant, bail,
    // otherwise we cannot analyze its output and it might be modified
    if (output->node()->kind() != prim::ListConstruct ||
        output->uses().size() != 1) {
      return c10::SymbolicShape();
    }
    Node* list_construct = output->node();
    std::vector<c10::optional<int64_t>> output_shape;
    for (Value* input : list_construct->inputs()) {
      output_shape.push_back(constant_as<int64_t>(input));
    }
    return c10::SymbolicShape(output_shape);
  }

  // node input indices that are TensorType and we need to iteratively
  // substitute properties of. We only substitute properties
  // of TensorTypes with a fixed dimension but not a complete shape,
  // because a complete shape we can completely replace with a constant
  // and non-fixed dimensions we cannot reason about at all
  // TODO: might be cleaner to store as a pair of index -> symbolic shape
  // but there were weird lifetime issues
  std::vector<int64_t> node_input_tensor_indices;
  std::shared_ptr<Graph> graph_;
  Node* node_;
};

void PropagateShapesWithShapeFunction(
    Node* n,
    std::shared_ptr<Graph>& shape_compute_graph) {
  c10::SymbolicShape out = SymbolicShapeAnalyzer(n, shape_compute_graph).run();
  n->output()->setType(
      n->output()->type()->expect<TensorType>()->withSymbolicShapes(out));
}

void RegisterOperatorShapeFunction(Node* n, std::shared_ptr<Graph>& graph) {
  std::lock_guard<std::mutex> guard(lock);
  if (!n->maybeSchema()) {
    return;
  }
  if (operator_functions.count(toString(n->schema()))) {
    return;
  }
  operator_functions[toString(n->schema())] = graph;
}

void PropagateShapesOnGraph(std::shared_ptr<Graph>& graph) {
  std::lock_guard<std::mutex> guard(lock);
  for (Node* n : graph->nodes()) {
    if (n->maybeSchema()) {
      if (operator_functions.count(toString(n->schema()))) {
        PropagateShapesWithShapeFunction(
            n, operator_functions[toString(n->schema())]);
      }
    }
  }
}

} // namespace jit
} // namespace torch
