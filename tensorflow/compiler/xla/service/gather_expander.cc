/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <utility>

#include "tensorflow/compiler/xla/service/gather_expander.h"
#include "tensorflow/compiler/xla/service/hlo_creation_utils.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/while_util.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/util.h"

namespace xla {
using tensorflow::gtl::ArraySlice;

static StatusOr<HloInstruction*> TransposeIndexVectorDimToLast(
    HloInstruction* gather_indices, int64 index_vector_dim) {
  const Shape& gather_indices_shape = gather_indices->shape();

  if (gather_indices_shape.dimensions_size() == index_vector_dim) {
    return gather_indices;
  }

  if (index_vector_dim == (gather_indices_shape.dimensions_size() - 1)) {
    return gather_indices;
  }

  std::vector<int64> permutation;
  permutation.reserve(gather_indices_shape.dimensions_size());
  for (int64 i = 0, e = gather_indices_shape.dimensions_size(); i < e; i++) {
    if (i != index_vector_dim) {
      permutation.push_back(i);
    }
  }
  permutation.push_back(index_vector_dim);
  return MakeTransposeHlo(gather_indices, permutation);
}

// Canonicalizes the gather_indices tensors so that we only have deal with some
// specific cases in the while loop that does the heavy lifting.
//
// See the "High Level Algorithm" section for a broader picture.
static StatusOr<HloInstruction*> CanonicalizeGatherIndices(
    HloInstruction* gather_indices, int64 index_vector_dim) {
  // Transpose the non-index-vector dimensions to the front.
  TF_ASSIGN_OR_RETURN(
      HloInstruction * transposed_gather_indices,
      TransposeIndexVectorDimToLast(gather_indices, index_vector_dim));
  bool indices_are_scalar =
      index_vector_dim == gather_indices->shape().dimensions_size();

  // The number of dimensions in gather_indices that are index dimensions.
  const int64 index_dims_in_gather_indices = indices_are_scalar ? 0 : 1;

  // If there is only one index (i.e. gather_indices has rank 1 and this gather
  // is really just a dynamic slice) add a leading degenerate dimension for
  // uniformity.  Otherwise create a "collapsed" leading dimension that subsumes
  // all of the non-index-vector dimensions.
  const Shape& shape = transposed_gather_indices->shape();
  if (shape.dimensions_size() == index_dims_in_gather_indices) {
    return PrependDegenerateDims(transposed_gather_indices, 1);
  } else {
    // Collapse all but the dimensions (0 or 1) in gather_indices containing the
    // index vectors.
    return CollapseFirstNDims(
        transposed_gather_indices,
        shape.dimensions_size() - index_dims_in_gather_indices);
  }
}

// Expands out or contracts away the gather dimensions in the accumulator
// produced by the while loop.
static StatusOr<HloInstruction*> AdjustGatherDimsInAccumulator(
    const Shape& gather_indices_shape, HloInstruction* accumulator,
    int64 index_vector_dim) {
  std::vector<int64> output_gather_dim_bounds;
  output_gather_dim_bounds.reserve(gather_indices_shape.dimensions_size());
  for (int64 i = 0, e = gather_indices_shape.dimensions_size(); i < e; i++) {
    if (i != index_vector_dim) {
      output_gather_dim_bounds.push_back(gather_indices_shape.dimensions(i));
    }
  }

  if (output_gather_dim_bounds.empty()) {
    // If output_gather_dim_bounds is empty we must be lowering a (effectively)
    // dynamic-slice.  In that case, there is a leading degenerate gather
    // dimension that we added to make this special case play well with the
    // general while loop which we need to remove now.
    return ElideDegenerateDims(accumulator, {0});
  }

  return ExpandFirstDimIntoNDims(accumulator, output_gather_dim_bounds);
}

// Expand an index vector from the gather_indices tensor into a vector that can
// be used to dynamic-slice out of the gather operand.
static StatusOr<HloInstruction*> ExpandIndexVectorIntoOperandSpace(
    HloInstruction* index_vector, const GatherDimensionNumbers& dim_numbers,
    int64 operand_rank) {
  HloComputation* computation = index_vector->parent();
  const Shape& index_shape = index_vector->shape();
  HloInstruction* zero =
      computation->AddInstruction(HloInstruction::CreateConstant(
          Literal::CreateFromDimensions(index_shape.element_type(), {1})));

  // We extract out individual components from the smaller index and concatenate
  // them (interspersing zeros as needed) into the larger index.
  std::vector<HloInstruction*> expanded_index_components;

  for (int i = 0; i < operand_rank; i++) {
    int64 index_vector_dim_index =
        FindIndex(dim_numbers.gather_dims_to_operand_dims(), i);
    if (index_vector_dim_index !=
        dim_numbers.gather_dims_to_operand_dims_size()) {
      TF_ASSIGN_OR_RETURN(
          HloInstruction * component_to_concat,
          MakeSliceHlo(index_vector, /*start_indices=*/{index_vector_dim_index},
                       /*limit_indices=*/{index_vector_dim_index + 1},
                       /*strides=*/{1}));
      expanded_index_components.push_back(component_to_concat);
    } else {
      expanded_index_components.push_back(zero);
    }
  }

  return MakeConcatHlo(expanded_index_components, /*dimension=*/0);
}

// This generates the body of the while that implements the main data movement
// behavior of gather using dynamic-slice and dynamic-update-slice.
static StatusOr<std::vector<HloInstruction*>> GatherLoopBody(
    const HloInstruction& gather, HloInstruction* induction_var,
    const std::vector<HloInstruction*>& incoming_loop_state) {
  const GatherDimensionNumbers& dim_numbers = gather.gather_dimension_numbers();
  CHECK_EQ(incoming_loop_state.size(), 3);
  HloInstruction* const operand = incoming_loop_state[0];
  HloInstruction* const gather_indices = incoming_loop_state[1];
  HloInstruction* const output_accumulator = incoming_loop_state[2];

  bool has_scalar_indices = gather_indices->shape().dimensions_size() == 1;
  CHECK_EQ(has_scalar_indices,
           dim_numbers.index_vector_dim() ==
               gather.operand(1)->shape().dimensions_size());

  TF_ASSIGN_OR_RETURN(
      HloInstruction * induction_var_as_vector,
      MakeBroadcastHlo(induction_var, /*broadcast_dimensions=*/{},
                       /*result_shape_bounds=*/{1}));

  HloInstruction* index_vector;

  if (has_scalar_indices) {
    // In this case gather_indices has rank 1 and induction_var_as_vector (of
    // shape {1}) is an index into this rank 1 tensor.
    TF_ASSIGN_OR_RETURN(
        index_vector,
        MakeDynamicSliceHlo(gather_indices, induction_var_as_vector, {1}));
  } else {
    // In this case gather_indices has rank 2 and induction_var_as_vector (of
    // shape {1}) is an index into just the first dimension of this rank 2
    // tensor.
    TF_ASSIGN_OR_RETURN(
        HloInstruction * index_into_gather_indices,
        PadVectorWithZeros(induction_var_as_vector,
                           /*zeros_to_prepend=*/0, /*zeros_to_append=*/1));

    int64 index_vector_size = gather_indices->shape().dimensions(1);
    TF_ASSIGN_OR_RETURN(
        HloInstruction * index_vector_2d,
        MakeDynamicSliceHlo(gather_indices, index_into_gather_indices,
                            {1, index_vector_size}));

    TF_ASSIGN_OR_RETURN(index_vector,
                        ElideDegenerateDims(index_vector_2d, {0}));
  }

  TF_ASSIGN_OR_RETURN(
      HloInstruction * gathered_slice_start,
      ExpandIndexVectorIntoOperandSpace(index_vector, dim_numbers,
                                        operand->shape().dimensions_size()));

  TF_ASSIGN_OR_RETURN(HloInstruction * gathered_slice,
                      MakeDynamicSliceHlo(operand, gathered_slice_start,
                                          gather.gather_window_bounds()));

  TF_ASSIGN_OR_RETURN(
      HloInstruction * gathered_slice_with_dims_elided,
      ElideDegenerateDims(gathered_slice,
                          AsInt64Slice(dim_numbers.elided_window_dims())));

  TF_ASSIGN_OR_RETURN(
      HloInstruction * gathered_slice_for_update,
      PrependDegenerateDims(gathered_slice_with_dims_elided, 1));

  TF_ASSIGN_OR_RETURN(
      HloInstruction * index_vector_into_accumulator,
      PadVectorWithZeros(
          induction_var_as_vector, /*zeros_to_prepend=*/0,
          /*zeros_to_append=*/
          gathered_slice_with_dims_elided->shape().dimensions_size()));

  TF_ASSIGN_OR_RETURN(
      HloInstruction * updated_accumulator,
      MakeDynamicUpdateSliceHlo(output_accumulator, gathered_slice_for_update,
                                index_vector_into_accumulator));

  // New loop state -- only the accumulator has changed.  The
  // WhileUtil::MakeCountedLoop functions takes care of the induction variable
  // and the while loop exit condition.
  return StatusOr<std::vector<HloInstruction*>>{
      {operand, gather_indices, updated_accumulator}};
}

static StatusOr<HloInstruction*> CreateGatherLoopAccumulatorInitValue(
    HloComputation* computation, PrimitiveType element_type,
    ArraySlice<int64> window_bounds, int64 gather_loop_trip_count,
    const GatherDimensionNumbers& dim_numbers) {
  std::vector<int64> accumulator_state_shape_dims;
  accumulator_state_shape_dims.reserve(1 + window_bounds.size());
  accumulator_state_shape_dims.push_back(gather_loop_trip_count);
  for (int64 i = 0; i < window_bounds.size(); i++) {
    if (!c_binary_search(dim_numbers.elided_window_dims(), i)) {
      accumulator_state_shape_dims.push_back(window_bounds[i]);
    }
  }
  return BroadcastZeros(computation, element_type,
                        accumulator_state_shape_dims);
}

// `accumulator` is almost the tensor the gather operation would have produced,
// except that it has the dimensions in the wrong order -- the gather dimensions
// are the major dimensions and the window dimensions are the minor dimensions.
// Fix this up with a transpose.
static StatusOr<HloInstruction*> PermuteGatherAndWindowDims(
    HloInstruction* accumulator, ArraySlice<int64> output_window_dims,
    int64 output_rank) {
  std::vector<int64> permutation;
  permutation.reserve(output_rank);

  int64 gather_idx_counter = 0;
  int64 window_idx_counter = output_rank - output_window_dims.size();
  for (int64 i = 0; i < output_rank; i++) {
    bool is_window_dim = c_binary_search(output_window_dims, i);
    if (is_window_dim) {
      permutation.push_back(window_idx_counter++);
    } else {
      permutation.push_back(gather_idx_counter++);
    }
  }

  return MakeTransposeHlo(accumulator, permutation);
}

// High Level Algorithm
//
// We follow the following steps in sequence:
//
//  1. We canonicalize the gather_indices tensor such that it has rank
//     2 (i.e. is a matrix) where each row is an index vector into the
//     operand.
//  2. We iterate over the set of indices in the canonicalized
//     gather_indices tensor using a while loop, accumulating slices
//     of the operand tensor into an accumulator using
//     DynamicUpdateSlice.
//  3. The accumulator result from the while loop from (2) is then
//     reshaped to split out all the individual gather dimensions and
//     then transposed to give the final result.
//
// As an example, if we started with the following operation:
//
//   HloModule TensorFlowGatherMultipleBatchDims
//
//   ENTRY main {
//     operand = s32[3,3] parameter(0)
//     indices = s32[2,2] parameter(1)
//     ROOT gather = s32[2,3,2] gather(operand, indices),
//         output_window_dims={1},
//         elided_window_dims={1},
//         gather_dims_to_operand_dims={1},
//         index_vector_dim=2,
//         window_bounds={3, 1}
//   }
//
// We'd first reshape indices to s32[4,1], where each row is an index
// into operand.  We'd then run a loop to slice out 4 tensors of shape
// [3,1] out of operand into an accumulator of shape [4,3,1].  We then
// reshape this result to [2,2,3] and finally transpose it to [2,3,2].

StatusOr<HloInstruction*> GatherExpander::ExpandGather(
    HloInstruction* gather_instr) {
  CHECK(!ShapeUtil::IsZeroElementArray(gather_instr->shape()));

  HloComputation* computation = gather_instr->parent();
  HloInstruction* operand = gather_instr->mutable_operand(0);
  HloInstruction* gather_indices = gather_instr->mutable_operand(1);
  const Shape& gather_indices_shape = gather_indices->shape();
  const Shape& output_shape = gather_instr->shape();
  int64 output_rank = output_shape.dimensions_size();

  const GatherDimensionNumbers& dim_numbers =
      gather_instr->gather_dimension_numbers();

  int64 gather_loop_trip_count = 1;
  for (int64 i = 0, e = gather_indices_shape.dimensions_size(); i < e; i++) {
    if (i != dim_numbers.index_vector_dim()) {
      gather_loop_trip_count *= gather_indices_shape.dimensions(i);
    }
  }

  if (!IsInt32(gather_loop_trip_count)) {
    return Unimplemented(
        "Gather operations with more than 2147483647 gather indices are not "
        "supported. This error occurred for %s.",
        gather_instr->ToString().c_str());
  }

  TF_ASSIGN_OR_RETURN(HloInstruction * canonical_gather_indices,
                      CanonicalizeGatherIndices(
                          gather_indices, dim_numbers.index_vector_dim()));

  CHECK_EQ(gather_loop_trip_count,
           canonical_gather_indices->shape().dimensions(0));

  TF_ASSIGN_OR_RETURN(
      HloInstruction * accumulator_init,
      CreateGatherLoopAccumulatorInitValue(
          computation, output_shape.element_type(),
          gather_instr->gather_window_bounds(), gather_loop_trip_count,
          gather_instr->gather_dimension_numbers()));

  StatusOr<std::vector<HloInstruction*>> gather_loop_result_or_error =
      WhileUtil::MakeCountedLoop(
          computation, gather_loop_trip_count,
          {operand, canonical_gather_indices, accumulator_init},
          [&](HloInstruction* indvar,
              const std::vector<HloInstruction*>& loop_state) {
            return GatherLoopBody(*gather_instr, indvar, loop_state);
          });

  TF_ASSIGN_OR_RETURN(std::vector<HloInstruction*> gather_loop_result,
                      gather_loop_result_or_error);

  HloInstruction* accumulator_result = gather_loop_result.back();

  TF_ASSIGN_OR_RETURN(
      HloInstruction * accumulator_with_output_gather_dims_decanonicalized,
      AdjustGatherDimsInAccumulator(gather_indices->shape(), accumulator_result,
                                    dim_numbers.index_vector_dim()));

  return PermuteGatherAndWindowDims(
      accumulator_with_output_gather_dims_decanonicalized,
      AsInt64Slice(dim_numbers.output_window_dims()), output_rank);
}

StatusOr<bool> GatherExpander::Run(HloModule* module) {
  auto is_nontrivial_gather = [](HloInstruction* inst) {
    return inst->opcode() == HloOpcode::kGather &&
           // Avoid expanding gather ops that produce zero sized tensors,
           // instead punt these to ZeroSizedHloElimination.
           !ShapeUtil::IsZeroElementArray(inst->shape());
  };

  std::vector<HloInstruction*> gather_instrs;
  for (HloComputation* computation : module->MakeNonfusionComputations()) {
    c_copy_if(computation->instructions(), std::back_inserter(gather_instrs),
              is_nontrivial_gather);
  }

  for (HloInstruction* inst : gather_instrs) {
    TF_ASSIGN_OR_RETURN(HloInstruction * expanded_root, ExpandGather(inst));
    TF_RETURN_IF_ERROR(inst->parent()->ReplaceInstruction(inst, expanded_root));
  }

  return !gather_instrs.empty();
}
}  // namespace xla
