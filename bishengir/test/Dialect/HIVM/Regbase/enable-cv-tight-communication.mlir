// REQUIRES: regbase
// TODO: enable this testcase after migrating disable-tight-coupled-buffer option

// Test the MixCV communication pass used when
// enable-cv-tight-communication=false.
// RUN: bishengir-opt \
// RUN: -hivm-insert-load-store-for-mix-cv="disable-tight-coupled-buffer=true" \
// RUN:   %s | FileCheck %s

// The communication mode is controlled by the user via the
// 'enable-cv-tight-communication' flag.

module attributes {hacc.target = #hacc.target<"Ascend950PR_9579">} {
  // CHECK-LABEL: func.func @test_fixpipe_to_vector(
  // CHECK-NOT: memref.alloc() : memref<16x16xf16, #hivm.address_space<ub>>
  // CHECK-NOT: memref.memory_space_cast
  // CHECK: hivm.hir.fixpipe
  // CHECK: hivm.hir.vmul

  func.func @test_fixpipe_to_vector(%src : tensor<16x16xf32>)
      -> tensor<16x16xf16> {
    %dst = tensor.empty() : tensor<16x16xf16>
    %fix = hivm.hir.fixpipe {dma_mode = #hivm.dma_mode<nz2nd>}
           ins(%src : tensor<16x16xf32>)
           outs(%dst : tensor<16x16xf16>) -> tensor<16x16xf16>

    %one = arith.constant 1.000000e+00 : f16
    %vdst = tensor.empty() : tensor<16x16xf16>
    %res = hivm.hir.vmul ins(%fix, %one : tensor<16x16xf16>, f16)
           outs(%vdst : tensor<16x16xf16>) -> tensor<16x16xf16>

    return %res : tensor<16x16xf16>
  }
}
