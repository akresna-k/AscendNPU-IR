//===- InsertCVTightCoupledBuffer.cpp ---------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
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
// This pass replaces marked tensor.empty operations with tightly coupled
// buffers.
//
//===----------------------------------------------------------------------===//

#include "bishengir/Dialect/HIVM/IR/HIVM.h"
#include "bishengir/Dialect/HIVM/Transforms/Passes.h"
#include "bishengir/Dialect/HIVM/Utils/Utils.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "llvm/ADT/STLExtras.h"

#include <tuple>

namespace mlir {
#define GEN_PASS_DEF_INSERTCVTIGHTCOUPLEDBUFFER
#include "bishengir/Dialect/HIVM/Transforms/Passes.h.inc"
} // namespace mlir

using namespace mlir;
using namespace mlir::hivm;

namespace {
struct InsertCVTightCoupledBufferPass
    : public impl::InsertCVTightCoupledBufferBase<
          InsertCVTightCoupledBufferPass> {
  using Base::Base;

  void runOnOperation() override;
  LogicalResult insertTightlyCoupledBuffer();
  LogicalResult verifyTightCoupledBufferInsertion(func::FuncOp funcOp) const;
};

/// Replaces inserted `tensor.empty` placeholders with tightly coupled buffers.
struct ReplaceEmptyOpByTightlyCoupledBuffer
    : public OpRewritePattern<tensor::EmptyOp> {
  using OpRewritePattern<tensor::EmptyOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::EmptyOp emptyOp,
                                PatternRewriter &rewriter) const override {
    if (!emptyOp->hasAttr(AddressSpaceAttr::name))
      return failure();

    auto addressSpace =
        cast<AddressSpaceAttr>(emptyOp->getAttr(AddressSpaceAttr::name));
    if (!emptyOp->hasAttr(hivm::kInsertedTensorAttr::name))
      return failure();

    for (OpOperand &use : llvm::make_early_inc_range(emptyOp->getUses())) {
      auto [plainMemref, toTensorOp] = insertTightCoupledBuffer(
          emptyOp, rewriter, addressSpace.getAddressSpace());
      use.assign(plainMemref);

      Operation *userOp = use.getOwner();
      rewriter.setInsertionPoint(userOp);
      rewriter.create(userOp->getLoc(),
                      StringAttr::get(rewriter.getContext(),
                                      userOp->getName().getStringRef()),
                      userOp->getOperands(), TypeRange{}, userOp->getAttrs());

      rewriter.replaceAllOpUsesWith(userOp, toTensorOp);
      rewriter.eraseOp(userOp);
    }

    return success();
  }

private:
  Value createAllocation(PatternRewriter &rewriter, tensor::EmptyOp emptyOp,
                         hivm::AddressSpace addressSpace) const {
    Value tensorValue = emptyOp.getResult();
    Location loc = emptyOp.getLoc();
    rewriter.setInsertionPointAfterValue(tensorValue);

    auto tensorType = cast<RankedTensorType>(tensorValue.getType());
    Value shapeSource = tensorValue;
    while (auto castOp = dyn_cast_or_null<UnrealizedConversionCastOp>(
               shapeSource.getDefiningOp()))
      shapeSource = castOp.getOperand(0);

    SmallVector<Value> dynamicSizes =
        hivm::getTensorDynamicValues(rewriter, loc, shapeSource);
    auto spaceAttr = AddressSpaceAttr::get(rewriter.getContext(), addressSpace);
    auto memrefType =
        MemRefType::get(tensorType.getShape(), tensorType.getElementType(),
                        /*layout=*/nullptr, spaceAttr);
    return createAllocWithMark(rewriter, loc, memrefType, dynamicSizes,
                               tensorType.getShape(),
                               tensorType.getElementType());
  }

  std::tuple<Value, bufferization::ToTensorOp>
  insertTightCoupledBuffer(tensor::EmptyOp emptyOp, PatternRewriter &rewriter,
                           hivm::AddressSpace addressSpace) const {
    auto resultType = cast<RankedTensorType>(emptyOp.getType());
    Value coupledBuffer = createAllocation(rewriter, emptyOp, addressSpace);
    auto toTensorOp = rewriter.create<bufferization::ToTensorOp>(
        emptyOp.getLoc(), resultType, coupledBuffer,
        /*restrict=*/true, /*writable=*/true);
    return std::make_tuple(coupledBuffer, toTensorOp);
  }
};
} // namespace

LogicalResult InsertCVTightCoupledBufferPass::verifyTightCoupledBufferInsertion(
    func::FuncOp funcOp) const {
  auto walkResult = funcOp->walk([](tensor::EmptyOp emptyOp) -> WalkResult {
    if (emptyOp->hasAttr(hivm::kInsertedTensorAttr::name))
      return WalkResult::interrupt();
    return WalkResult::advance();
  });
  return failure(walkResult.wasInterrupted());
}

LogicalResult InsertCVTightCoupledBufferPass::insertTightlyCoupledBuffer() {
  RewritePatternSet patterns(&getContext());
  patterns.add<ReplaceEmptyOpByTightlyCoupledBuffer>(&getContext());
  if (failed(applyPatternsGreedily(getOperation(), std::move(patterns))))
    return failure();

  return verifyTightCoupledBufferInsertion(getOperation());
}

void InsertCVTightCoupledBufferPass::runOnOperation() {
  if (failed(insertTightlyCoupledBuffer()))
    return signalPassFailure();

  if (onlyInsertTightlyCoupledBuffer)
    return;
}

std::unique_ptr<Pass> mlir::hivm::createInsertCVTightCoupledBufferPass(
    const InsertCVTightCoupledBufferOptions &options) {
  return std::make_unique<InsertCVTightCoupledBufferPass>(options);
}
