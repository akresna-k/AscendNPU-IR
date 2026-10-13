//===- InsertLoadStoreForMixCV.cpp ------------------------------*- C++ -*-===//
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
// This pass inserts load/store op for mix cv function.
//
//===----------------------------------------------------------------------===//

#include "bishengir/Dialect/HACC/Utils/Utils.h"
#include "bishengir/Dialect/HIVM/IR/HIVM.h"
#include "bishengir/Dialect/HIVM/IR/HIVMImpl.h"
#include "bishengir/Dialect/HIVM/Transforms/InsertLoadStoreForMixCV/InsertPropagation.h"
#include "bishengir/Dialect/HIVM/Transforms/InsertLoadStoreForMixCV/PropagateOp.h"
#include "bishengir/Dialect/HIVM/Transforms/InsertLoadStoreForMixCV/ResolvePropagation.h"
#include "bishengir/Dialect/HIVM/Transforms/InsertLoadStoreForMixCV/Utils.h"
#include "bishengir/Dialect/HIVM/Transforms/Passes.h"
#include "bishengir/Dialect/HIVM/Utils/Utils.h"
#include "bishengir/Dialect/Scope/IR/Scope.h"
#include "bishengir/Dialect/Tensor/Transforms/Passes.h"
#include "bishengir/Dialect/Utils/Util.h"

#include "bishengir/Transforms/Passes.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/Value.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/Passes.h"

#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/LogicalResult.h"

namespace mlir {
#define GEN_PASS_DEF_INSERTLOADSTOREFORMIXCV
#include "bishengir/Dialect/HIVM/Transforms/Passes.h.inc"
} // namespace mlir

#define DEBUG_TYPE "insert-load-store"
#define DBGS() (llvm::dbgs() << "[" DEBUG_TYPE "]: ")
#define LDBG(X) LLVM_DEBUG(DBGS() << X << "\n")

using namespace mlir;
using namespace mlir::hivm;

namespace {
static const llvm::StringLiteral kConvertLayoutOperand =
    "converted-layout-operand";

llvm::SmallVector<size_t> getIntegersOfArrayAttr(Operation *op,
                                                 llvm::StringLiteral attrName) {
  ArrayAttr arr = op->getAttrOfType<ArrayAttr>(attrName);
  llvm::SmallVector<size_t> result;
  if (!arr)
    return result;
  result.reserve(arr.size());
  for (Attribute attr : arr) {
    auto intAttr = dyn_cast<IntegerAttr>(attr);
    // In production you may want to assert or handle errors.
    assert(intAttr && "ArrayAttr element is not an IntegerAttr");
    // Use getValue().getZExtValue() to interpret as unsigned.
    result.push_back(intAttr.getValue().getZExtValue());
  }
  return result;
}

void insertToIntegerAttrOfArrayAttr(Operation *op, llvm::StringLiteral attrName,
                                    size_t value, PatternRewriter &rewriter) {
  auto arr = getIntegersOfArrayAttr(op, attrName);
  if (std::find(arr.begin(), arr.end(), value) != arr.end())
    return;
  arr.push_back(value);
  std::sort(arr.begin(), arr.end());

  SmallVector<Attribute> newAttrs;
  for (auto idx : arr) {
    newAttrs.push_back(
        rewriter.getIntegerAttr(rewriter.getIntegerType(64), idx));
  }
  op->setAttr(attrName, rewriter.getArrayAttr(newAttrs));
}

struct InsertLoadStoreForMixCVPass
    : public impl::InsertLoadStoreForMixCVBase<InsertLoadStoreForMixCVPass> {
public:
  using Base::Base;
  void runOnOperation() override;

private:
  bool isA5Target();
  bool isEnabledTightCoupledBuffer();
  SmallVector<PropagationStep> getPropagationSteps();
  LogicalResult insertPropagationOp(func::FuncOp funcOp);
  LogicalResult propagateAndResolve(func::FuncOp funcOp);
  LogicalResult runPropagateOpPatterns(func::FuncOp funcOp,
                                       PropagationStep step);
  LogicalResult addConvertLayoutUBToL1(func::FuncOp funcOp);
  LogicalResult setOperationsCoreType(OpBuilder builder, func::FuncOp funcOp);
};

bool InsertLoadStoreForMixCVPass::isA5Target() {
  auto moduleOp = getOperation()->getParentOfType<ModuleOp>();
  auto moduleTarget = hacc::utils::getTargetDevice(moduleOp);
  if (moduleTarget.has_value())
    return hacc::utils::isAscend950(moduleTarget.value());
  return hacc::utils::isAscend950(this->target);
}

bool InsertLoadStoreForMixCVPass::isEnabledTightCoupledBuffer() {
  if (disableTightCoupledBuffer || !isA5Target())
    return false;
  // Ascend950: L0C-to-Ub and UB-to-L1 requires CV12.
  // Disable this path for CV11
  auto func = getOperation();
  if (auto ratio = getCoreRatioAttr(func))
    return ratio.getCube() != 1 || ratio.getVector() != 1;
  return true;
}

} // namespace

LogicalResult
InsertLoadStoreForMixCVPass::runPropagateOpPatterns(func::FuncOp funcOp,
                                                    PropagationStep step) {
  RewritePatternSet patterns(funcOp.getContext());
  GreedyRewriteConfig rewriteConfig;
  patterns.add<PropagateUpPattern>(patterns.getContext(), step, isA5Target());
  patterns.add<PropagateDownPattern>(patterns.getContext(), step);
  if (step != PropagationStep::LOCAL && step != PropagationStep::ALL)
    patterns.add<ControlFlowPropagatePattern>(patterns.getContext(), step);
  patterns.add<ResolvePropagationPattern, RemoveRedundantPropagationPattern>(
      patterns.getContext());
  rewriteConfig.fold = false;

  if (isEnabledTightCoupledBuffer()) {
    patterns.add<TightCoupledBufferResolvePropagationPattern>(
        patterns.getContext(), inferFixpipeDmaMode);
  }

  if (failed(
          applyPatternsGreedily(funcOp, std::move(patterns), rewriteConfig))) {
    return failure();
  }
  LDBG("After propagation step " << static_cast<int>(step));
  LDBG(funcOp);
  return success();
}

LogicalResult
InsertLoadStoreForMixCVPass::insertPropagationOp(func::FuncOp funcOp) {
  IRRewriter rewriter(funcOp.getContext());
  rewriter.setInsertionPointToStart(&funcOp.getBody().front());
  for (auto arg : funcOp.getArguments()) {
    if (isa<ShapedType>(arg.getType())) {
      Value newArg = PropagatorUtil::createPropagator(
          arg, kPropagateDownAttr, hivm::AddressSpace::GM, rewriter);
      rewriter.replaceAllUsesExcept(arg, newArg, newArg.getDefiningOp());
    }
  }

  RewritePatternSet patterns(funcOp.getContext());
  GreedyRewriteConfig rewriteConfig;
  patterns.add<InsertPropagationPattern>(patterns.getContext());
  if (isA5Target())
    patterns.add<A5InsertionPattern>(patterns.getContext());
  if (isEnabledTightCoupledBuffer()) {
    patterns.add<TightCoupledBufferInsertionPattern>(patterns.getContext());
  }
  // TODO: add case for InsertUBAfterFixpipePattern
  // case of InsertL1BeforeMmadPattern tested by
  // compile-triton-hstu-attn-fwd.mlir
  rewriteConfig.fold = false;
  if (failed(
          applyPatternsGreedily(funcOp, std::move(patterns), rewriteConfig))) {
    return failure();
  }
  return success();
}

SmallVector<PropagationStep>
InsertLoadStoreForMixCVPass::getPropagationSteps() {
  return {PropagationStep::L0C, PropagationStep::LOCAL, PropagationStep::UB,
          PropagationStep::L1, PropagationStep::ALL};
}

LogicalResult
InsertLoadStoreForMixCVPass::propagateAndResolve(func::FuncOp funcOp) {
  /// prioritize L0C to resolve propagation after the scf.if. so, only 1 fixpipe
  /// is required.
  /// %mmad = hivm.hir.mmadL1
  ///  %if = scf.if ... {
  ///    %another_mmad = hivm.hir.mmadL1
  ///    scf.yield %another_mmad
  ///  } else {
  ///    scf.yield %mmad
  ///  }
  ///  hivm.hir.vadd ins(%mmad, %mmad)
  SmallVector<PropagationStep> propagations = getPropagationSteps();
  for (auto step : propagations) {
    if (failed(runPropagateOpPatterns(funcOp, step))) {
      return failure();
    }
  }
  RewritePatternSet patterns(funcOp.getContext());
  GreedyRewriteConfig rewriteConfig;
  patterns.add<CloneMultipleAddressSpaceOperation>(patterns.getContext());
  rewriteConfig.fold = false;
  if (failed(
          applyPatternsGreedily(funcOp, std::move(patterns), rewriteConfig))) {
    return failure();
  }
  LDBG("After clone multiple address space: " << funcOp);
  if (failed(runPropagateOpPatterns(funcOp, PropagationStep::ALL))) {
    return failure();
  }
  return success();
}

namespace mlir::hivm {

constexpr static llvm::StringLiteral maybeUnCollapsibleReshape =
    "maybeUnCollapsibleReshape";

static uint64_t getElemBytesForAlign(Type t) {
  if (auto ft = dyn_cast<FloatType>(t))
    return (uint64_t)((ft.getWidth() + 7) / 8);
  if (auto it = dyn_cast<IntegerType>(t))
    return (uint64_t)((it.getWidth() + 7) / 8);
  if (isa<IndexType>(t))
    return 8ULL;
  if (auto ct = dyn_cast<ComplexType>(t))
    return 2ULL * getElemBytesForAlign(ct.getElementType());
  return 0ULL;
}

static FailureOr<uint64_t> getBlockElemsFor32BAlign(Type elemType) {
  constexpr uint64_t kAlignBytes = 32;
  uint64_t elemBytes = getElemBytesForAlign(elemType);
  if (elemBytes <= 0)
    return failure();
  if (elemBytes >= kAlignBytes)
    return 1;
  if (kAlignBytes % elemBytes != 0)
    return failure();
  return kAlignBytes / elemBytes;
}

static void ensureAllocInUbAddressSpaceIfNeeded(PatternRewriter &rewriter,
                                                memref::AllocOp allocOp) {
  auto oldType = dyn_cast<MemRefType>(allocOp.getMemref().getType());
  if (!oldType)
    return;
  auto maybeSpace = mlir::hivm::getOptionalHIVMAddressSpace(oldType);
  if (maybeSpace.has_value())
    return;

  MLIRContext *ctx = rewriter.getContext();
  auto ubSpaceAttr = hivm::AddressSpaceAttr::get(ctx, hivm::AddressSpace::UB);
  auto newType =
      mlir::MemRefType::get(oldType.getShape(), oldType.getElementType(),
                            oldType.getLayout(), ubSpaceAttr);

  SmallVector<OpOperand *> originalUses;
  for (OpOperand &use : allocOp.getMemref().getUses())
    originalUses.push_back(&use);

  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.modifyOpInPlace(allocOp,
                           [&]() { allocOp.getMemref().setType(newType); });

  rewriter.setInsertionPointAfter(allocOp);
  Value replacement = allocOp.getMemref();
  if (replacement.getType() != oldType) {
    replacement = rewriter.create<memref::MemorySpaceCastOp>(
        allocOp.getLoc(), oldType, replacement);
  }
  for (OpOperand *use : originalUses)
    use->set(replacement);
}

/// pattern2
/// %53 = hivm.hir.vcast ins(%42 : tensor<16x16xf32>) outs(%19 : ...) -> ...
/// %54 = tensor.empty() : tensor<16x16xf32>
/// %55 = hivm.hir.mmadL1 ins(%53, %52, %true, %c16, %c16, %c16 : ...
///
/// is converted into
///
/// %53 = hivm.hir.vcast ins(%42 : tensor<16x16xf32>) outs(%19 : ...) -> ...
/// %alloc_0 = memref.alloc() : memref<..., #hivm.address_space<cbuf>>
/// %memspacecast_2 = memref.memory_space_cast %alloc_0
/// : memref<..., #hivm.address_space<cbuf>> to ...
/// %empty = bufferization.to_tensor %memspacecast_2 restrict writable : ...
/// %copy = hivm.hir.copy ins(%53 : ...) outs(%empty : ...) -> ...
/// %54 = tensor.empty() : tensor<16x16xf32>
/// %55 = hivm.hir.mmadL1 ins(%copy, %52, %true, %c16, %c16, %c16 : ...

LogicalResult
insertConvertLayout(PatternRewriter &rewriter, hivm::MmadL1Op mmadOp,
                    const llvm::SmallVector<OpOperand *> &consumerOperands) {
  if (consumerOperands.empty()) {
    return failure();
  }

  bool changed = false;
  for (OpOperand *consumerOperand : consumerOperands) {
    Value origTensor = consumerOperand->get();
    // TODO: enhance support for dynamic shape
    auto tensorType = mlir::dyn_cast<RankedTensorType>(origTensor.getType());
    if (!tensorType)
      continue;

    Operation *consumerOp = consumerOperand->getOwner();
    Location loc = consumerOp->getLoc();
    rewriter.setInsertionPoint(consumerOp);

    // TODO: Consider encapsulating it as an nd2dz function
    int64_t M = tensorType.getDimSize(0);
    int64_t N = tensorType.getDimSize(1);
    bool isA = (origTensor == mmadOp.getA());
    bool isTranspose = isA ? mmadOp.getATranspose().has_value()
                           : mmadOp.getBTranspose().has_value();
    auto blockSizes = mmadOp.getBlockSizesTile(origTensor, isTranspose, isA);
    int32_t alignM = static_cast<int32_t>(blockSizes[0]);
    int32_t alignN = static_cast<int32_t>(blockSizes[1]);
    auto elemType = tensorType.getElementType();
    int64_t newN = static_cast<int64_t>(
        AlignUp(static_cast<uint64_t>(N), static_cast<uint64_t>(alignN)));
    if ((M != ShapedType::kDynamic && (M % alignM)) || (newN != N)) {
      int64_t newM = static_cast<int64_t>(
          AlignUp(static_cast<uint64_t>(M), static_cast<uint64_t>(alignM)));
      auto paddedType = RankedTensorType::get({newM, newN}, elemType);
      Value zeroConst = rewriter.create<arith::ConstantOp>(
          loc, elemType, rewriter.getZeroAttr(elemType));

      Value emptyForVbrc = rewriter.create<tensor::EmptyOp>(
          loc, paddedType.getShape(), elemType);
      auto vbrcOp = rewriter.create<hivm::VBrcOp>(loc, paddedType, zeroConst,
                                                  emptyForVbrc);
      Value initializedMatrix = vbrcOp->getResult(0);
      SmallVector<OpFoldResult> offsets = {rewriter.getIndexAttr(0),
                                           rewriter.getIndexAttr(0)};
      SmallVector<OpFoldResult> sizes = {rewriter.getIndexAttr(M),
                                         rewriter.getIndexAttr(N)};
      SmallVector<OpFoldResult> strides = {rewriter.getIndexAttr(1),
                                           rewriter.getIndexAttr(1)};
      origTensor = rewriter.create<tensor::InsertSliceOp>(
          loc, origTensor, initializedMatrix, offsets, sizes, strides);
      tensorType = mlir::cast<RankedTensorType>(origTensor.getType());
      M = newM;
      N = newN;
    }

    SmallVector<ReassociationIndices> reassociation = {{0}, {1, 2}};
    auto blkOr = getBlockElemsFor32BAlign(elemType);
    if (failed(blkOr)) {
      return consumerOp->emitOpError()
             << "unsupported element type for 32B-aligned expand_shape: "
             << elemType;
    }
    int64_t blk = (int64_t)*blkOr;

    int64_t M1 = M / alignM;
    // TODO: enhance UB alignment
    int64_t N1 = N / blk;
    auto dstTy = RankedTensorType::get({M, N1, blk}, elemType);
    auto expandOp = rewriter.create<tensor::ExpandShapeOp>(
        loc, dstTy, origTensor, reassociation);
    auto emptyTensorType = RankedTensorType::get({N1, M, blk}, elemType);
    auto emptyTransposed = rewriter.create<tensor::EmptyOp>(
        loc, emptyTensorType.getShape(), emptyTensorType.getElementType());
    SmallVector<int64_t> premVec = {1, 0, 2};
    auto transposed = rewriter.create<hivm::VTransposeOp>(
        loc, emptyTransposed->getResultTypes(), expandOp.getResult(),
        emptyTransposed.getResult(), rewriter.getDenseI64ArrayAttr(premVec));
    auto nzTy = RankedTensorType::get({N1, M1, alignM, blk}, elemType);
    SmallVector<ReassociationIndices> nzReassoc = {{0}, {1, 2}, {3}};
    auto nzOp = rewriter.create<tensor::ExpandShapeOp>(
        loc, nzTy, transposed->getResult(0), nzReassoc);
    rewriter.modifyOpInPlace(consumerOp, [&]() { consumerOperand->set(nzOp); });
    changed = true;
  }
  return changed ? success() : failure();
}

static bool isVFCall(Operation *op) {
  if (auto callOp = dyn_cast<func::CallOp>(op)) {
    if (callOp->hasAttr(hivm::VectorFunctionAttr::name))
      return true;
  }
  return false;
}

template <typename... OpTypes> static bool traceVector(Value v) {
  return ((!std::is_same_v<OpTypes, hivm::VBrcOp> &&
           traceDefOp<OpTypes>(v) != std::nullopt) ||
          ...);
}

template <typename OpType>
struct AddConvertLayoutUBToL1 : public OpRewritePattern<hivm::MmadL1Op> {
  using OpRewritePattern<hivm::MmadL1Op>::OpRewritePattern;
  ~AddConvertLayoutUBToL1() override = default;
  LogicalResult matchAndRewrite(hivm::MmadL1Op op,
                                PatternRewriter &rewriter) const override {
    bool changed = false;
    for (OpOperand *operand : op.getDpsInputOperands()) {
      Value beforeValue = operand->get();
      auto operandIdx = operand->getOperandNumber();
      auto inserted = getIntegersOfArrayAttr(op, kConvertLayoutOperand);
      if (std::find(inserted.begin(), inserted.end(), operandIdx) !=
          inserted.end())
        continue;
      auto producerOps = traceDefOps<OpType>(beforeValue);
      if (std::is_same_v<OpType, hivm::VBrcOp>)
        continue;
      if (producerOps.empty())
        continue;

      bool matched = false;
      for (Operation *producer : producerOps) {
        if constexpr (std::is_same_v<OpType, mlir::scf::ForOp>) {
          auto scfForOp = llvm::cast<mlir::scf::ForOp>(producer);
          if (!scfForOp->hasAttr(ExtractLoadStoreAttr)) {
            continue;
          }
          auto res = cast<OpResult>(beforeValue);
          auto yieldOp = scfForOp.getBody()->getTerminator();
          auto yieldOpOperand = yieldOp->getOperand(res.getResultNumber());
          auto vectorOp = traceVector<
#define GET_OP_LIST
#include "bishengir/Dialect/HIVM/IR/HIVMVectorOps.cpp.inc"
              >(yieldOpOperand);
          if (!vectorOp)
            continue;
        }
        if constexpr (std::is_same_v<OpType, func::CallOp>) {
          if (!isVFCall(producer))
            continue;
        }
        if constexpr (std::is_same_v<OpType, memref::AllocOp>) {
          auto allocOp = llvm::cast<memref::AllocOp>(producer);
          auto maybeSpace = mlir::hivm::getOptionalHIVMAddressSpace(
              allocOp.getMemref().getType());
          if (!maybeSpace.has_value() ||
              maybeSpace.value() != hivm::AddressSpace::UB)
            continue;
        }
        if constexpr (std::is_same_v<OpType, bufferization::ToTensorOp>) {
          auto toTensorOp = llvm::cast<bufferization::ToTensorOp>(producer);
          auto maybeAnnotateOp = utils::getAnnotateOpWithAttr(
              toTensorOp.getResult(), kMayImplicitTransposeWithLastAxis);
          if (!maybeAnnotateOp.has_value()) {
            maybeAnnotateOp = utils::getAnnotateOpWithAttr(
                toTensorOp.getMemref(), kMayImplicitTransposeWithLastAxis);
          }
          if (!maybeAnnotateOp.has_value())
            continue;
        }
        if constexpr (std::is_same_v<OpType, tensor::CollapseShapeOp>) {
          auto collapseShapeOp = llvm::cast<tensor::CollapseShapeOp>(producer);
          auto maybeAnnotateOp = utils::getAnnotateOpWithAttr(
              collapseShapeOp.getResult(), maybeUnCollapsibleReshape);
          if (!maybeAnnotateOp.has_value())
            continue;
        }
        if constexpr (std::is_same_v<OpType, tensor::InsertSliceOp>) {
          // if the source is dynamic shape then, it can only be handled in
          // vector core
          auto insertSliceOp = llvm::cast<tensor::InsertSliceOp>(producer);
          auto rankedTensorType =
              dyn_cast<RankedTensorType>(insertSliceOp.getSource().getType());
          if (!rankedTensorType || rankedTensorType.hasStaticShape())
            continue;
        }
        matched = true;
        break;
      }
      if (!matched)
        continue;

      auto allocOps = traceDefOps<memref::AllocOp>(beforeValue);
      llvm::SmallVector<OpOperand *> consumerOperands{operand};
      bool isBiasOperand = (beforeValue == op.getPerChannelBias());
      auto tensorType = mlir::dyn_cast<RankedTensorType>(beforeValue.getType());
      // Only rank-2 tensors (original ND format) need to be transposed.
      // After ND2NZ conversion the tensor becomes rank-4 (NZ format), so
      // transposition is not required.
      bool needTransposed = (tensorType.getRank() == 2);
      if (isBiasOperand || !needTransposed)
        continue;
      LogicalResult result =
          insertConvertLayout(rewriter, op, consumerOperands);
      if (failed(result))
        continue;
      insertToIntegerAttrOfArrayAttr(op, kConvertLayoutOperand, operandIdx,
                                     rewriter);

      // Handle case where multiple operands of `op` are same SSA value.
      // e.g., `hivm.hir.mmadL1 ins(%vec, %vec, ...)` where both operands are
      // the same vector. In this case, we only want to rewrite the operand
      // once, and make sure that other operand is updated with the new value as
      // well.
      Value converted = operand->get();
      rewriter.modifyOpInPlace(op, [&beforeValue, &converted, &op,
                                    &rewriter]() {
        // Use for loop to insert operand index for `kConvertLayoutOperand`
        for (OpOperand *operand : op.getDpsInputOperands()) {
          if (operand->get() != beforeValue)
            continue;
          op->setOperand(operand->getOperandNumber(), converted);
          insertToIntegerAttrOfArrayAttr(op, kConvertLayoutOperand,
                                         operand->getOperandNumber(), rewriter);
        }
      });

      for (Operation *allocOp : allocOps)
        ensureAllocInUbAddressSpaceIfNeeded(
            rewriter, llvm::cast<memref::AllocOp>(allocOp));
      changed = true;
    }
    return changed ? success() : failure();
  }
};

// Handle convert layout from ub to L1 for FixpipeOp->MmadL1Op.
template <>
struct AddConvertLayoutUBToL1<hivm::FixpipeOp>
    : public OpRewritePattern<hivm::MmadL1Op> {
  using OpRewritePattern<hivm::MmadL1Op>::OpRewritePattern;
  ~AddConvertLayoutUBToL1() override = default;
  LogicalResult matchAndRewrite(hivm::MmadL1Op op,
                                PatternRewriter &rewriter) const override {
    bool changed = false;

    for (OpOperand &operand : op->getOpOperands()) {
      auto operandIdx = operand.getOperandNumber();
      auto inserted = getIntegersOfArrayAttr(op, kConvertLayoutOperand);
      if (std::find(inserted.begin(), inserted.end(), operandIdx) !=
          inserted.end())
        continue;
      auto maybeFixpipe = traceDefOp<hivm::FixpipeOp>(operand.get());
      if (!maybeFixpipe)
        continue;

      // FixpipeOp of rank 4 (Fractal layout) do not need add layout
      // conversion.
      auto fixpipeOp = llvm::cast<hivm::FixpipeOp>(*maybeFixpipe);
      // don't insert convert layout for FixpipeOp that is used by MmadL1Op
      // with NZ2NZ DMA mode
      if (fixpipeOp.getDmaMode() == FixpipeDMAMode::NZ2NZ) {
        continue;
      }
      auto fixpipeType = cast<ShapedType>(fixpipeOp->getResult(0).getType());
      if (fixpipeType.hasRank() && fixpipeType.getRank() == 4)
        continue;

      llvm::SmallVector<OpOperand *> consumerOperands{&operand};

      Value valueAfterUb = operand.get();

      bool isBiasOperand = (operand.get() == op.getPerChannelBias());
      if (isBiasOperand)
        continue;
      LogicalResult l1Result =
          insertConvertLayout(rewriter, op, consumerOperands);
      if (failed(l1Result))
        continue;
      insertToIntegerAttrOfArrayAttr(op.getOperation(), kConvertLayoutOperand,
                                     operand.getOperandNumber(), rewriter);

      Value convertedToL1 = operand.get();
      if (valueAfterUb != convertedToL1) {
        rewriter.modifyOpInPlace(
            op, [&]() { op->replaceUsesOfWith(valueAfterUb, convertedToL1); });
      }
      changed = true;
    }
    return changed ? success() : failure();
  }
};

// trace uppropOp, check if it's mmadop

} // namespace mlir::hivm

template <typename... OpTypes>
void populateAddConvertLayoutUBToL1(RewritePatternSet &patterns) {
  (patterns.add<AddConvertLayoutUBToL1<OpTypes>>(patterns.getContext()), ...);
}

LogicalResult
InsertLoadStoreForMixCVPass::addConvertLayoutUBToL1(func::FuncOp funcOp) {
  RewritePatternSet patterns(funcOp.getContext());
  GreedyRewriteConfig rewriteConfig;
  populateAddConvertLayoutUBToL1<
      tensor::InsertSliceOp, hivm::FixpipeOp, func::CallOp, mlir::scf::ForOp,
      hivm::IndirectLoadOp, hivm::StrideLoadOp, mlir::hivm::GatherLoadOp,
      mlir::scope::ScopeOp, tensor::CollapseShapeOp, bufferization::ToTensorOp,
      memref::AllocOp,
#define GET_OP_LIST
#include "bishengir/Dialect/HIVM/IR/HIVMVectorOps.cpp.inc"
      >(patterns);
  rewriteConfig.fold = false;
  if (failed(
          applyPatternsGreedily(funcOp, std::move(patterns), rewriteConfig))) {
    return failure();
  }

  funcOp->walk(
      [](hivm::MmadL1Op op) -> void { op->removeAttr(kConvertLayoutOperand); });
  return success();
}

static LogicalResult setOperationsCoreTypeForA5(OpBuilder builder,
                                                func::FuncOp funcOp) {
  /// Postprocess of adding core type attribute
  funcOp->walk([&builder](Operation *op) {
    TypeSwitch<Operation *>(op)
        .Case([&](hivm::DebugOp op) {
          auto upProp = PropagatorUtil::getUpPropagator(&op.getArgMutable());
          if (upProp) {
            auto coreType = PropagatorUtil::getCoreType(upProp);
            auto addressSpaces = PropagatorUtil::getAddressSpace(upProp);
            if (!addressSpaces.empty()) {
              auto memScopeAttr = hivm::AddressSpaceAttr::get(op.getContext(),
                                                              addressSpaces[0]);
              op.setMemscopeAttr(memScopeAttr);
            }
            if (coreType != TCoreType::CUBE_AND_VECTOR) {
              op.setTcoretypeAttr(
                  TCoreTypeAttr::get(op.getContext(), coreType));
            } else {
              auto addressSpace = addressSpaces.empty() ? hivm::AddressSpace::UB
                                                        : addressSpaces[0];
              op.setTcoretypeAttr(TCoreTypeAttr::get(
                  op.getContext(),
                  PropagatorUtil::kAddressSpace2CoreType.at(addressSpace)));
            }
          }
        })
        .Case([&](hivm::LoadOp op) {
          auto upProp = PropagatorUtil::getUpPropagator(&op.getDstMutable());
          TCoreTypeAttr currentTcoretype = op.getTcoretype();
          if (!upProp)
            return;

          // shouldn't change the core type that has been set
          // consider case of vtranspose user of load op
          if (currentTcoretype.getTcoretype() != TCoreType::CUBE_OR_VECTOR)
            return;

          auto inferNewCoreType =
              [&op](UnrealizedConversionCastOp upProp) -> TCoreTypeAttr {
            auto coreType = PropagatorUtil::getCoreType(upProp);
            if (coreType != TCoreType::CUBE_AND_VECTOR) {
              return TCoreTypeAttr::get(op.getContext(), coreType);
            } else {
              auto addressSpaces = PropagatorUtil::getAddressSpace(upProp);
              if (addressSpaces.empty()) {
                LDBG("not set tcoretype for " << op);
                return nullptr;
              }
              auto addressSpace = addressSpaces[0];
              return TCoreTypeAttr::get(
                  op.getContext(),
                  PropagatorUtil::kAddressSpace2CoreType.at(addressSpace));
            }
          };

          TCoreTypeAttr newTcoretype = inferNewCoreType(upProp);
          // in A5, load op with cube core type will be converted to nd2nz
          // from combining load and convert_layout
          if (newTcoretype &&
              newTcoretype.getTcoretype() == TCoreType::VECTOR) {
            LDBG("set tcoretype for " << op << " to " << newTcoretype);
            op.setTcoretypeAttr(newTcoretype);
          }
        })
        .Case([&](hivm::VBrcOp vbrcOp) {
          auto upProp =
              PropagatorUtil::getUpPropagator(&vbrcOp.getDstMutable());
          TCoreTypeAttr currentTcoretype =
              vbrcOp->getAttrOfType<hivm::TCoreTypeAttr>(
                  hivm::TCoreTypeAttr::name);
          if (!upProp)
            return;

          // shouldn't change the core type that has been set
          // consider case of vtranspose user of vbrcOp
          if (currentTcoretype &&
              currentTcoretype.getTcoretype() != TCoreType::CUBE_OR_VECTOR)
            return;

          auto inferNewCoreType =
              [&vbrcOp](UnrealizedConversionCastOp upProp) -> TCoreTypeAttr {
            auto coreType = PropagatorUtil::getCoreType(upProp);
            if (coreType != TCoreType::CUBE_AND_VECTOR &&
                coreType != TCoreType::CUBE_OR_VECTOR) {
              return TCoreTypeAttr::get(vbrcOp.getContext(), coreType);
            } else {
              auto addressSpaces = PropagatorUtil::getAddressSpace(upProp);
              if (addressSpaces.empty()) {
                LDBG("not set tcoretype for " << vbrcOp);
                return nullptr;
              }
              auto addressSpace = addressSpaces[0];
              return TCoreTypeAttr::get(
                  vbrcOp.getContext(),
                  PropagatorUtil::kAddressSpace2CoreType.at(addressSpace));
            }
          };

          TCoreTypeAttr newTcoretype = inferNewCoreType(upProp);
          if (newTcoretype) {
            LDBG("set tcoretype for " << vbrcOp << " to " << newTcoretype);
            vbrcOp->setAttr(hivm::TCoreTypeAttr::name,
                            builder.getAttr<hivm::TCoreTypeAttr>(
                                newTcoretype.getTcoretype()));
          }
        })
        .Case([&](tensor::ExtractOp op) {
          auto upProp = PropagatorUtil::getUpPropagator(&op.getTensorMutable());
          if (!upProp)
            return;

          auto coreType = PropagatorUtil::getCoreType(upProp);
          if (coreType != TCoreType::CUBE_AND_VECTOR) {
            op->setAttr(hivm::TCoreTypeAttr::name,
                        TCoreTypeAttr::get(op.getContext(), coreType));
          } else {
            auto addressSpaces = PropagatorUtil::getAddressSpace(upProp);
            auto addressSpace = addressSpaces.empty() ? hivm::AddressSpace::UB
                                                      : addressSpaces[0];
            op->setAttr(
                hivm::TCoreTypeAttr::name,
                TCoreTypeAttr::get(
                    op.getContext(),
                    PropagatorUtil::kAddressSpace2CoreType.at(addressSpace)));
          }
        });
  });
  return success();
}

static LogicalResult setOperationsCoreTypeForA3(OpBuilder builder,
                                                func::FuncOp funcOp) {
  /// Postprocess of adding core type attribute
  funcOp->walk([](Operation *op) {
    TypeSwitch<Operation *>(op)
        .Case([&](hivm::DebugOp op) {
          auto upProp = PropagatorUtil::getUpPropagator(&op.getArgMutable());
          if (upProp) {
            auto coreType = PropagatorUtil::getCoreType(upProp);
            auto addressSpaces = PropagatorUtil::getAddressSpace(upProp);
            if (!addressSpaces.empty()) {
              auto memScopeAttr = hivm::AddressSpaceAttr::get(op.getContext(),
                                                              addressSpaces[0]);
              op.setMemscopeAttr(memScopeAttr);
            }
            if (coreType != TCoreType::CUBE_AND_VECTOR) {
              op.setTcoretypeAttr(
                  TCoreTypeAttr::get(op.getContext(), coreType));
            } else {
              auto addressSpace = addressSpaces.empty() ? hivm::AddressSpace::UB
                                                        : addressSpaces[0];
              op.setTcoretypeAttr(TCoreTypeAttr::get(
                  op.getContext(),
                  PropagatorUtil::kAddressSpace2CoreType.at(addressSpace)));
            }
          }
        })
        .Case([&](hivm::LoadOp op) {
          auto upProp = PropagatorUtil::getUpPropagator(&op.getDstMutable());
          if (upProp) {
            auto coreType = PropagatorUtil::getCoreType(upProp);
            if (coreType != TCoreType::CUBE_AND_VECTOR) {
              op.setTcoretypeAttr(
                  TCoreTypeAttr::get(op.getContext(), coreType));
            } else {
              auto addressSpaces = PropagatorUtil::getAddressSpace(upProp);
              auto addressSpace = addressSpaces.empty() ? hivm::AddressSpace::UB
                                                        : addressSpaces[0];
              op.setTcoretypeAttr(TCoreTypeAttr::get(
                  op.getContext(),
                  PropagatorUtil::kAddressSpace2CoreType.at(addressSpace)));
            }
          }
        })
        .Case([&](tensor::ExtractOp op) {
          auto upProp = PropagatorUtil::getUpPropagator(&op.getTensorMutable());
          if (!upProp)
            return;

          auto coreType = PropagatorUtil::getCoreType(upProp);
          if (coreType != TCoreType::CUBE_AND_VECTOR) {
            op->setAttr(hivm::TCoreTypeAttr::name,
                        TCoreTypeAttr::get(op.getContext(), coreType));
          } else {
            auto addressSpaces = PropagatorUtil::getAddressSpace(upProp);
            auto addressSpace = addressSpaces.empty() ? hivm::AddressSpace::UB
                                                      : addressSpaces[0];
            op->setAttr(
                hivm::TCoreTypeAttr::name,
                TCoreTypeAttr::get(
                    op.getContext(),
                    PropagatorUtil::kAddressSpace2CoreType.at(addressSpace)));
          }
        });
  });
  return success();
}

LogicalResult
InsertLoadStoreForMixCVPass::setOperationsCoreType(OpBuilder builder,
                                                   func::FuncOp funcOp) {
  if (isA5Target()) {
    return setOperationsCoreTypeForA5(builder, funcOp);
  }
  return setOperationsCoreTypeForA3(builder, funcOp);
}

void InsertLoadStoreForMixCVPass::runOnOperation() {
  OpBuilder builder(&getContext());
  auto *ctx = &getContext();
  auto funcOp = getOperation();

  if (!hacc::utils::isDeviceEntry(funcOp))
    return;

  PassManager pm(ctx);
  if (!isA5Target())
    pm.addPass(tensor::createReplicateOutEmptyTensorPass());

  if (isEnabledTightCoupledBuffer() && failed(addConvertLayoutUBToL1(funcOp))) {
    return signalPassFailure();
  }

  LDBG("After adding convert layout UB to L1 Op: " << funcOp);

  if (failed(pm.run(funcOp))) {
    return signalPassFailure();
  }
  // To maintain propagator, rewriteConfig.fold should be false
  if (failed(insertPropagationOp(funcOp))) {
    return signalPassFailure();
  }
  LDBG("After inserting Propagation Op: " << funcOp);
  if (failed(propagateAndResolve(funcOp))) {
    return signalPassFailure();
  }
  LDBG("After propagation & resolve: " << funcOp);
  LLVM_DEBUG({
    if (failed(verifyPropagation(funcOp))) {
      return signalPassFailure();
    }
  });

  if (failed(setOperationsCoreType(builder, funcOp))) {
    return signalPassFailure();
  }

  PassManager pm2(ctx);
  pm2.addPass(bishengir::createExtendedCanonicalizerPass());
  if (failed(pm2.run(funcOp))) {
    return signalPassFailure();
  }

  PassManager pm3(ctx);
  pm3.addPass(createInsertLoadStoreForScalarPass());
  if (failed(pm3.run(funcOp))) {
    return signalPassFailure();
  }
}

std::unique_ptr<Pass> mlir::hivm::createInsertLoadStoreForMixCVPass(
    const InsertLoadStoreForMixCVOptions &options) {
  return std::make_unique<InsertLoadStoreForMixCVPass>(options);
}
