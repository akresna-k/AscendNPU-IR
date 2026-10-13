//===- PropagatorCoreTypeTest.cpp - mix-cv propagator core type tests -----===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// InsertLoadStoreForMixCV encodes two distinct propagator roles:
//   * CUBE_AND_VECTOR is stored on a GM propagator.
//   * CUBE_OR_VECTOR is the default core type of an
//     unrealized_conversion_cast and is omitted from the marker.
//

#include "gtest/gtest.h"

#include "bishengir/Dialect/HIVM/IR/HIVM.h"
#include "bishengir/Dialect/HIVM/Transforms/InsertLoadStoreForMixCV/Utils.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/PatternMatch.h"

using namespace mlir;
using namespace mlir::hivm;

namespace {

class PropagatorCoreTypeTest : public ::testing::Test {
protected:
  void SetUp() override {
    context.loadDialect<func::FuncDialect, hivm::HIVMDialect>();
    builder = std::make_unique<OpBuilder>(&context);
    rewriter = std::make_unique<PatternRewriter>(&context);
    module = ModuleOp::create(builder->getUnknownLoc());
    builder->setInsertionPointToStart(module->getBody());

    auto loc = builder->getUnknownLoc();
    auto tensorType = RankedTensorType::get({4, 4}, builder->getF16Type());
    auto func = builder->create<func::FuncOp>(
        loc, "kernel", builder->getFunctionType({tensorType}, {}));
    func.addEntryBlock();
    Block &entry = func.getBody().front();
    arg = entry.getArgument(0);
    builder->setInsertionPointToStart(&entry);
    builder->create<func::ReturnOp>(loc);
    builder->setInsertionPoint(entry.getTerminator());
    rewriter->setInsertionPoint(entry.getTerminator());
  }

  UnrealizedConversionCastOp asCast(Value value) {
    auto cast = value.getDefiningOp<UnrealizedConversionCastOp>();
    EXPECT_TRUE(static_cast<bool>(cast));
    return cast;
  }

  void expectStoredCoreType(UnrealizedConversionCastOp cast,
                            TCoreType expected) {
    ASSERT_TRUE(static_cast<bool>(cast));
    EXPECT_EQ(PropagatorUtil::getCoreType(cast), expected);
    EXPECT_EQ(PropagatorUtil::extractPropagatorInfo(cast).first, expected);
    auto attr = cast->getAttrOfType<TCoreTypeAttr>(hivm::TCoreTypeAttr::name);
    ASSERT_TRUE(attr);
    EXPECT_EQ(attr.getTcoretype(), expected);
  }

  void expectDefaultCoreType(UnrealizedConversionCastOp cast) {
    ASSERT_TRUE(static_cast<bool>(cast));
    EXPECT_EQ(PropagatorUtil::getCoreType(cast), TCoreType::CUBE_OR_VECTOR);
    EXPECT_EQ(PropagatorUtil::extractPropagatorInfo(cast).first,
              TCoreType::CUBE_OR_VECTOR);
    EXPECT_FALSE(cast->hasAttr(hivm::TCoreTypeAttr::name));
  }

  MLIRContext context;
  std::unique_ptr<OpBuilder> builder;
  std::unique_ptr<PatternRewriter> rewriter;
  OwningOpRef<ModuleOp> module;
  Value arg;
};

TEST_F(PropagatorCoreTypeTest, GMAddressSpaceMapsToCubeAndVector) {
  EXPECT_EQ(PropagatorUtil::kAddressSpace2CoreType.at(AddressSpace::GM),
            TCoreType::CUBE_AND_VECTOR);
  EXPECT_EQ(PropagatorUtil::kAddressSpace2CoreType.at(AddressSpace::UB),
            TCoreType::VECTOR);
  EXPECT_EQ(PropagatorUtil::kAddressSpace2CoreType.at(AddressSpace::L1),
            TCoreType::CUBE);
  EXPECT_EQ(PropagatorUtil::kAddressSpace2CoreType.at(AddressSpace::L0C),
            TCoreType::CUBE);
}

TEST_F(PropagatorCoreTypeTest, MTE2PipeUsesGMPropagator) {
  auto [coreType, addressSpace] =
      PropagatorUtil::getPropagationInfoForPipe(PIPE::PIPE_MTE2);
  EXPECT_EQ(coreType, TCoreType::CUBE_AND_VECTOR);
  EXPECT_EQ(addressSpace, AddressSpace::GM);

  std::tie(coreType, addressSpace) =
      PropagatorUtil::getPropagationInfoForPipe(PIPE::PIPE_S);
  EXPECT_EQ(coreType, TCoreType::CUBE_AND_VECTOR);
  EXPECT_EQ(addressSpace, AddressSpace::GM);

  std::tie(coreType, addressSpace) =
      PropagatorUtil::getPropagationInfoForPipe(PIPE::PIPE_V);
  EXPECT_EQ(coreType, TCoreType::VECTOR);
  EXPECT_EQ(addressSpace, AddressSpace::UB);
}

TEST_F(PropagatorCoreTypeTest, GMPropagatorStoresCubeAndVector) {
  Value propagated = PropagatorUtil::createPropagator(
      arg, kPropagateDownAttr, AddressSpace::GM, *builder);
  auto cast = asCast(propagated);
  expectStoredCoreType(cast, TCoreType::CUBE_AND_VECTOR);
  auto spaces = PropagatorUtil::getAddressSpace(cast);
  ASSERT_EQ(spaces.size(), 1u);
  EXPECT_EQ(spaces[0], AddressSpace::GM);
}

TEST_F(PropagatorCoreTypeTest, DefaultPropagatorOmitsCubeOrVector) {
  Value propagated = PropagatorUtil::createPropagator(
      arg, kPropagateDownAttr, TCoreType::CUBE_OR_VECTOR, *builder);
  expectDefaultCoreType(asCast(propagated));
}

TEST_F(PropagatorCoreTypeTest, ExplicitCubeAndVectorIsStored) {
  Value propagated = PropagatorUtil::createPropagator(
      arg, kPropagateDownAttr, TCoreType::CUBE_AND_VECTOR, *builder);
  expectStoredCoreType(asCast(propagated), TCoreType::CUBE_AND_VECTOR);
}

TEST_F(PropagatorCoreTypeTest, NonGMAddressSpaceKeepsDefaultCoreType) {
  Value propagated = PropagatorUtil::createPropagator(
      arg, kPropagateDownAttr, AddressSpace::UB, *builder);
  auto cast = asCast(propagated);
  expectDefaultCoreType(cast);
  auto spaces = PropagatorUtil::getAddressSpace(cast);
  ASSERT_EQ(spaces.size(), 1u);
  EXPECT_EQ(spaces[0], AddressSpace::UB);
}

TEST_F(PropagatorCoreTypeTest, UpdatingToDefaultClearsStoredCoreType) {
  Value propagated = PropagatorUtil::createPropagator(
      arg, kPropagateDownAttr, AddressSpace::GM, *builder);
  auto cast = asCast(propagated);
  expectStoredCoreType(cast, TCoreType::CUBE_AND_VECTOR);

  PropagatorUtil::createPropagatorDown(cast.getResult(0),
                                       TCoreType::CUBE_OR_VECTOR,
                                       AddressSpace::GM, *rewriter);
  expectDefaultCoreType(cast);
  auto spaces = PropagatorUtil::getAddressSpace(cast);
  ASSERT_EQ(spaces.size(), 1u);
  EXPECT_EQ(spaces[0], AddressSpace::GM);

  PropagatorUtil::createPropagatorDown(cast.getResult(0),
                                       TCoreType::CUBE_AND_VECTOR,
                                       AddressSpace::GM, *rewriter);
  expectStoredCoreType(cast, TCoreType::CUBE_AND_VECTOR);
}

} // namespace
