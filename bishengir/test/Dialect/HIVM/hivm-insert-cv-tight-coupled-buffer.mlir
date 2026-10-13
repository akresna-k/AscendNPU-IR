// RUN: bishengir-opt --hivm-insert-cv-tight-coupled-buffer -split-input-file %s | FileCheck %s

// CHECK-LABEL: func.func @replace_marked_empty(
// CHECK-SAME: %[[SOURCE:.*]]: tensor<16x16xf16>
func.func @replace_marked_empty(%source : tensor<16x16xf16>)
    -> tensor<16x16xf16> {
  // CHECK: %[[ALLOC:.*]] = memref.alloc() : memref<16x16xf16, #hivm.address_space<cbuf>>
  // CHECK: annotation.mark %[[ALLOC]] {buffer_size_in_byte = 512 : i64}
  // CHECK: %[[TENSOR:.*]] = bufferization.to_tensor %[[ALLOC]] restrict writable
  // CHECK: hivm.hir.copy ins(%[[SOURCE]] : tensor<16x16xf16>) outs(%[[ALLOC]] : memref<16x16xf16, #hivm.address_space<cbuf>>)
  // CHECK: return %[[TENSOR]] : tensor<16x16xf16>
  %empty = tensor.empty() {
    hivm.address_space = #hivm.address_space<cbuf>,
    "hivm.inserted-tensor"
  } : tensor<16x16xf16>
  %result = hivm.hir.copy
      ins(%source : tensor<16x16xf16>)
      outs(%empty : tensor<16x16xf16>) -> tensor<16x16xf16>
  return %result : tensor<16x16xf16>
}

// -----

// Verify that the pass no longer inserts data movement for unmarked tensors.
// CHECK-LABEL: func.func @leave_unmarked_empty_unchanged(
// CHECK-NOT: memref.alloc
// CHECK: %[[EMPTY:.*]] = tensor.empty() : tensor<16x16xf16>
// CHECK: %[[RESULT:.*]] = hivm.hir.fixpipe
// CHECK-SAME: outs(%[[EMPTY]] : tensor<16x16xf16>)
func.func @leave_unmarked_empty_unchanged(%source : tensor<16x16xf32>)
    -> tensor<16x16xf16> {
  %empty = tensor.empty() : tensor<16x16xf16>
  %result = hivm.hir.fixpipe {
      dma_mode = #hivm.dma_mode<nz2nd>,
      pre_quant = #hivm.fixpipe_pre_quant_mode<F322F16>
    } ins(%source : tensor<16x16xf32>)
      outs(%empty : tensor<16x16xf16>) -> tensor<16x16xf16>
  return %result : tensor<16x16xf16>
}
