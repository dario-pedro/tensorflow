/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/service/hlo_scheduling.h"

#include <memory>
#include <string>

#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/service/hlo_ordering.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"
#include "tensorflow/compiler/xla/tools/parser/hlo_parser.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"

namespace xla {
namespace {

class MinimumMemoryForSequenceTest : public HloTestBase {};

TEST_F(MinimumMemoryForSequenceTest, MultiComputation) {
  auto module = CreateNewModule();
  const Shape scalar_shape = ShapeUtil::MakeShape(xla::F32, {});
  const Shape tuple_shape =
      ShapeUtil::MakeTupleShape({scalar_shape, scalar_shape});

  auto cond_builder = HloComputation::Builder("WhileCond");
  // Tuple param: 24 bytes (each elem has 8 byte pointer, 4 byte element)
  HloInstruction* cond_param = cond_builder.AddInstruction(
      HloInstruction::CreateParameter(0, tuple_shape, "cond_param"));
  HloInstruction* cond_iter = cond_builder.AddInstruction(
      HloInstruction::CreateGetTupleElement(scalar_shape, cond_param, 0));
  HloInstruction* cond_data = cond_builder.AddInstruction(
      HloInstruction::CreateGetTupleElement(scalar_shape, cond_param, 1));
  // Free cond_param[] (16 bytes), Alloc PRED[] (1 byte)
  HloInstruction* cond_lt = cond_builder.AddInstruction(
      HloInstruction::CreateBinary(ShapeUtil::MakeShape(PRED, {}),
                                   HloOpcode::kLt, cond_iter, cond_data));
  HloComputation* cond_computation =
      module->AddEmbeddedComputation(cond_builder.Build());

  auto body_builder = HloComputation::Builder("WhileBody");
  // Tuple param: 24 bytes (each elem has 8 byte pointer, 4 byte element)
  HloInstruction* body_param = body_builder.AddInstruction(
      HloInstruction::CreateParameter(0, tuple_shape, "body_param"));
  HloComputation* body_computation =
      module->AddEmbeddedComputation(body_builder.Build());

  auto builder = HloComputation::Builder(TestName());
  // Entry params: 8 bytes (4 bytes per param), TOTAL=8
  HloInstruction* iter = builder.AddInstruction(
      HloInstruction::CreateParameter(0, scalar_shape, "param_iter"));
  HloInstruction* data = builder.AddInstruction(
      HloInstruction::CreateParameter(1, scalar_shape, "param_data"));
  // Tuple: 16 bytes (8 bytes per pointer), TOTAL=24
  HloInstruction* tuple =
      builder.AddInstruction(HloInstruction::CreateTuple({iter, data}));
  // While: 8 bytes (4 bytes per element), TOTAL=32
  // Both cond and body use a max of 24 bytes, TOTAL=56
  HloInstruction* while_op = builder.AddInstruction(HloInstruction::CreateWhile(
      tuple_shape, cond_computation, body_computation, tuple));
  HloComputation* entry_computation =
      module->AddEntryComputation(builder.Build());

  auto size_fn = [](const BufferValue& buffer) {
    return ShapeUtil::ByteSizeOf(buffer.shape(), /*pointer_size=*/8);
  };

  SequentialHloOrdering::HloModuleSequence module_sequence;
  module_sequence[cond_computation] = {cond_param, cond_iter, cond_data,
                                       cond_lt};
  module_sequence[body_computation] = {body_param};
  module_sequence[entry_computation] = {iter, data, tuple, while_op};
  EXPECT_EQ(56,
            MinimumMemoryForSequence(module_sequence, size_fn).ValueOrDie());
}

class HloSchedulingTest : public HloTestBase {};

TEST_F(HloSchedulingTest, LastUseScheduledFirst) {
  // Tests scheduling of the following HLO code:
  //
  //   %ab = abs(%param)
  //   %exp = exp(%param)
  //   %add = add(%ab, %exp)
  //   %negate = negate(%exp)
  //   %sub = subtract(%add, %negate)
  //
  // %add should be scheduled before %negate because %add is the last (and only)
  // use of %ab. Scheduling %add first then frees up %ab's buffer.
  const Shape vec = ShapeUtil::MakeShape(xla::F32, {42});
  auto builder = HloComputation::Builder(TestName());
  auto param =
      builder.AddInstruction(HloInstruction::CreateParameter(0, vec, "param"));
  auto ab = builder.AddInstruction(
      HloInstruction::CreateUnary(vec, HloOpcode::kAbs, param));
  auto exp = builder.AddInstruction(
      HloInstruction::CreateUnary(vec, HloOpcode::kExp, param));

  auto add = builder.AddInstruction(
      HloInstruction::CreateBinary(vec, HloOpcode::kAdd, ab, exp));
  auto negate = builder.AddInstruction(
      HloInstruction::CreateUnary(vec, HloOpcode::kNegate, exp));
  auto sub = builder.AddInstruction(
      HloInstruction::CreateBinary(vec, HloOpcode::kSubtract, add, negate));

  auto module = CreateNewModule();
  module->AddEntryComputation(builder.Build());

  TF_ASSERT_OK_AND_ASSIGN(
      SequentialHloOrdering::HloModuleSequence sequence,
      CreateMemoryMinimizingSequence(*module, [](const BufferValue& buffer) {
        return ShapeUtil::ByteSizeOf(buffer.shape());
      }));
  // Verify that all instructions are in the sequence.
  EXPECT_EQ(module->entry_computation()->instruction_count(),
            sequence.at(module->entry_computation()).size());

  // The first instruction should be the parameter and the last the root "sub".
  EXPECT_EQ(param, sequence.at(module->entry_computation()).front());
  EXPECT_EQ(sub, sequence.at(module->entry_computation()).back());

  SequentialHloOrdering ordering(module.get(), sequence);
  EXPECT_TRUE(ordering.ExecutesBefore(add, negate));
}

TEST_F(HloSchedulingTest, ListSchedulerHandlesAliasing) {
  const char* module_str = R"(
HloModule test_aliasing_module

ENTRY root {
  param = s32[1000] parameter(0)
  p0 = s32[1000] copy(param)
  p1 = s32[1000] copy(param)
  t = (s32[1000], s32[1000]) tuple(p0, p1)
  a = s32[1000] get-tuple-element(t), index=0
  b = s32[1000] get-tuple-element(t), index=1
  c = s32[1000] add(a, b)
  d = s32[1000] add(c, b)
  e = s32[1000] add(c, c)
  f = s32[1000] add(e, e)
  ROOT result = (s32[1000], s32[1000], s32[1000]) tuple(d, e, f)
})";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          tools::Parse(module_str));

  auto size_fn = [](const BufferValue& buffer) {
    return ShapeUtil::ByteSizeOf(buffer.shape(), /*pointer_size=*/8);
  };
  TF_ASSERT_OK_AND_ASSIGN(
      SequentialHloOrdering::HloModuleSequence sequence,
      CreateMemoryMinimizingSequence(*module, size_fn, ListMemoryScheduler));
  // Verify that all instructions are in the sequence.
  EXPECT_EQ(module->entry_computation()->instruction_count(),
            sequence.at(module->entry_computation()).size());

  std::unordered_map<string, const HloInstruction*> instructions_by_name;
  for (const HloInstruction* instruction :
       sequence.at(module->entry_computation())) {
    instructions_by_name[instruction->name()] = instruction;
  }

  // The first instruction should be the parameter and the last the root.
  EXPECT_EQ(instructions_by_name.at("param"),
            sequence.at(module->entry_computation()).front());
  EXPECT_EQ(instructions_by_name.at("result"),
            sequence.at(module->entry_computation()).back());

  // Instructions "d" and "e" will both be schedulable at the same time, but
  // instruction "d" allows us to free the buffer of "p1", so the list scheduler
  // should prefer it.
  SequentialHloOrdering ordering(module.get(), sequence);
  EXPECT_TRUE(ordering.ExecutesBefore(instructions_by_name.at("d"),
                                      instructions_by_name.at("e")));
}

TEST_F(HloSchedulingTest, ListAccountsForSubcomputations) {
  // %WhileCond (cond_param: f32[4]) -> pred[] {
  //   %cond_param = f32[4]{0} parameter(0)
  //   %constant = f32[1,4]{1,0} constant(f32[1,4] { { 0, 0, 0, 0 } })
  //   ROOT %not-equal-to = pred[] not-equal-to(
  //     f32[4]{0} %cond_param, f32[1,4]{1,0} %constant)
  // }
  // %WhileBody (body_param: f32[4]) -> f32[4] {
  //   %body_param = f32[4]{0} parameter(0)
  //   %constant.1 = f32[1,4]{1,0} constant(f32[1,4] { { 1, 1, 1, 1 } })
  //   ROOT %subtract = f32[4]{0} subtract(
  //     f32[4]{0} %body_param, f32[1,4]{1,0} %constant.1)
  // }
  // %SubcomputationsNotAccounted () -> f32[2,4] {
  //   %constant.3 = f32[2,4]{1,0} constant(
  //     f32[2,4] { { 1, 2, 3, 4 }, { 1, 2, 3, 4 } })
  //   %transpose = f32[2,4]{1,0} transpose(
  //     f32[2,4]{1,0} %constant.3), dimensions={0,1}
  //   %constant.2 = f32[1,4]{1,0} constant(f32[1,4] { { 1, 1, 1, 1 } })
  //   %while = f32[4]{0} while(f32[1,4]{1,0} %constant.2),
  //      condition=%WhileCond,
  //      body=%WhileBody
  //   %broadcast = f32[2,4]{1,0} broadcast(f32[4]{0} %while), dimensions={0}
  //   ROOT %add = f32[2,4]{1,0} add(
  //     f32[2,4]{1,0} %transpose, f32[2,4]{1,0} %broadcast)
  // }

  auto module = CreateNewModule();
  const Shape r1f32 = ShapeUtil::MakeShape(F32, {4});
  const Shape r2f32 = ShapeUtil::MakeShape(F32, {2, 4});

  // param != 0
  // Needs 17 bytes
  auto cond_builder = HloComputation::Builder("WhileCond");
  HloInstruction* cond_param = cond_builder.AddInstruction(
      HloInstruction::CreateParameter(0, r1f32, "cond_param"));
  HloInstruction* zero_vector = cond_builder.AddInstruction(
      HloInstruction::CreateConstant(Literal::CreateR2<float>({{0, 0, 0, 0}})));
  cond_builder.AddInstruction(HloInstruction::CreateBinary(
      ShapeUtil::MakeShape(PRED, {}), HloOpcode::kNe, cond_param, zero_vector));
  auto cond_computation = module->AddEmbeddedComputation(cond_builder.Build());

  // param - 1
  // Needs 16 bytes
  auto body_builder = HloComputation::Builder("WhileBody");
  HloInstruction* body_param = body_builder.AddInstruction(
      HloInstruction::CreateParameter(0, r1f32, "body_param"));
  HloInstruction* one_vector = body_builder.AddInstruction(
      HloInstruction::CreateConstant(Literal::CreateR2<float>({{1, 1, 1, 1}})));
  body_builder.AddInstruction(HloInstruction::CreateBinary(
      r1f32, HloOpcode::kSubtract, body_param, one_vector));
  auto body_computation = module->AddEmbeddedComputation(body_builder.Build());

  // transpose(matrix) + bcast(while)
  auto builder = HloComputation::Builder(TestName());
  HloInstruction* while_init = builder.AddInstruction(
      HloInstruction::CreateConstant(Literal::CreateR2<float>({{1, 1, 1, 1}})));
  // Creates 16 bytes, ignoring subcomputations
  HloInstruction* while_loop =
      builder.AddInstruction(HloInstruction::CreateWhile(
          r1f32, cond_computation, body_computation, while_init));

  // Creates 32 bytes and frees 16
  HloInstruction* bcast = builder.AddInstruction(
      HloInstruction::CreateBroadcast(r2f32, while_loop, {0}));

  HloInstruction* matrix = builder.AddInstruction(
      HloInstruction::CreateConstant(Literal::CreateR2<float>(
          {{1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}})));
  // Creates 32 bytes
  HloInstruction* transpose = builder.AddInstruction(
      HloInstruction::CreateTranspose(r2f32, matrix, {0, 1}));

  // Creates 32 bytes and frees 64
  HloInstruction* add = builder.AddInstruction(
      HloInstruction::CreateBinary(r2f32, HloOpcode::kAdd, transpose, bcast));

  module->AddEntryComputation(builder.Build());

  TF_ASSERT_OK_AND_ASSIGN(SequentialHloOrdering::HloModuleSequence sequence,
                          CreateMemoryMinimizingSequence(
                              *module,
                              [](const BufferValue& buffer) {
                                return ShapeUtil::ByteSizeOf(buffer.shape());
                              },
                              ListMemoryScheduler));
  // Verify that all instructions are in the sequence.
  EXPECT_EQ(module->entry_computation()->instruction_count(),
            sequence.at(module->entry_computation()).size());
  SequentialHloOrdering ordering(module.get(), sequence);
  // This schedule is an example of List's greedy heuristics being suboptimal.
  // The while_loop is more expensive than transpose, so it would have been
  // better to schedule it first, instead of during the busy time.
  EXPECT_TRUE(ordering.ExecutesBefore(transpose, while_loop));
  EXPECT_TRUE(ordering.ExecutesBefore(transpose, bcast));
  EXPECT_TRUE(ordering.ExecutesBefore(bcast, add));
  EXPECT_TRUE(ordering.ExecutesBefore(transpose, add));
}

}  // namespace
}  // namespace xla
