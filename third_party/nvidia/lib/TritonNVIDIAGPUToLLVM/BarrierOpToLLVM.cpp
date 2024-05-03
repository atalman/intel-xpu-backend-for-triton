/*
 * Copyright (c) 2023 NVIDIA Corporation & Affiliates. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "PatternTritonGPUOpToLLVM.h"
#include "TritonNVIDIAGPUToLLVM/PTXAsmFormat.h"
#include "mlir/Conversion/LLVMCommon/Pattern.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "triton/Conversion/TritonGPUToLLVM/Utility.h"

#include "Utility.h"

using namespace mlir;
using namespace mlir::triton;

namespace {
struct BarrierOpConversion
    : public ConvertOpToLLVMPattern<mlir::gpu::BarrierOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(mlir::gpu::BarrierOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    if (op->hasAttr("bar_id")) {
      // llvm.nvvm.barrier0 doesn't support bar_id and num_threads attributes,
      // so we have to lower it to ptx manually.
      auto barId = op->getAttrOfType<IntegerAttr>("bar_id").getInt();
      auto numThreads = op->getAttrOfType<IntegerAttr>("num_threads").getInt();
      barSync(rewriter, op, barId, numThreads);
      rewriter.eraseOp(op);
      return success();
    }
    // Otherwise we let the default lowering handle it
    return failure();
  }
};

struct FenceAsyncSharedOpConversion
    : public ConvertOpToLLVMPattern<triton::nvidia_gpu::FenceAsyncSharedOp> {
  using ConvertOpToLLVMPattern<
      triton::nvidia_gpu::FenceAsyncSharedOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::nvidia_gpu::FenceAsyncSharedOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    rewriter.replaceOpWithNewOp<triton::nvgpu::FenceAsyncSharedOp>(
        op, adaptor.getBCluster());
    return success();
  }
};

struct InitBarrierOpConversion
    : public ConvertOpToLLVMPattern<triton::nvidia_gpu::InitBarrierOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::nvidia_gpu::InitBarrierOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    auto smemObj = LLVM::getSharedMemoryObjectFromStruct(
        loc, adaptor.getAlloc(),
        typeConverter->convertType(op.getAlloc().getType().getElementType()),
        rewriter);

    auto id = getThreadId(rewriter, loc);
    auto pred = icmp_eq(id, i32_val(0));
    ::mlir::triton::PTXBuilder ptxBuilder;
    const std::string ptx = "@$0 mbarrier.init.shared::cta.b64 [$1], " +
                            std::to_string(op.getCount()) + ";";
    auto &barSyncOp = *ptxBuilder.create<>(ptx);
    barSyncOp({ptxBuilder.newOperand(pred, "b"),
               ptxBuilder.newOperand(smemObj.getBase(), "r")},
              /*onlyAttachMLIRArgs=*/true);
    auto voidTy = void_ty(op->getContext());
    ptxBuilder.launch(rewriter, loc, voidTy);
    rewriter.eraseOp(op);
    return success();
  }
};

struct WaitBarrierOpConversion
    : public ConvertOpToLLVMPattern<triton::nvidia_gpu::WaitBarrierOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::nvidia_gpu::WaitBarrierOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto smemObj = LLVM::getSharedMemoryObjectFromStruct(
        op.getLoc(), adaptor.getAlloc(),
        typeConverter->convertType(op.getAlloc().getType().getElementType()),
        rewriter);
    auto loc = op.getLoc();
    const std::string ptx =
        "{                                                           \n\t"
        ".reg .pred P1;                                              \n\t"
        "waitLoop:                                                   \n\t"
        "mbarrier.try_wait.parity.shared.b64 P1, [$0], $1;           \n\t"
        "@!P1 bra.uni waitLoop;                                      \n\t"
        "}                                                           \n\t";
    ::mlir::triton::PTXBuilder ptxBuilder;
    auto &waitLoop = *ptxBuilder.create<>(ptx);
    waitLoop({ptxBuilder.newOperand(smemObj.getBase(), "r"),
              ptxBuilder.newOperand(adaptor.getPhase(), "r")},
             /*onlyAttachMLIRArgs=*/true);
    auto voidTy = void_ty(op->getContext());
    ptxBuilder.launch(rewriter, op->getLoc(), voidTy);
    barrier();
    rewriter.eraseOp(op);
    return success();
  }
};
} // namespace

void mlir::triton::NVIDIA::populateBarrierOpToLLVMPatterns(
    LLVMTypeConverter &typeConverter, RewritePatternSet &patterns,
    PatternBenefit benefit) {
  patterns.add<BarrierOpConversion>(typeConverter, benefit);
  patterns.add<FenceAsyncSharedOpConversion>(typeConverter, benefit);
  patterns.add<InitBarrierOpConversion>(typeConverter, benefit);
  patterns.add<WaitBarrierOpConversion>(typeConverter, benefit);
}
