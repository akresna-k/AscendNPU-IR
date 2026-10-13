// RUN: bishengir-opt -hivm-insert-load-store-for-mix-cv %s | FileCheck %s

// A GM function argument is reached by a GM propagator. That propagator
// stores CUBE_AND_VECTOR, so debug copies CUBE_AND_VECTOR. Mapping GM back
// to CUBE_OR_VECTOR stamps the omitted default instead.
// CHECK-LABEL: func.func @debug_gm_arg
// CHECK: hivm.hir.debug
// CHECK-SAME: memscope = #hivm.address_space<gm>
// CHECK-SAME: tcoretype = #hivm.tcore_type<CUBE_AND_VECTOR>
// CHECK-NOT: CUBE_OR_VECTOR
func.func @debug_gm_arg(%arg0: tensor<16x16xf16>) attributes {hacc.entry, hacc.function_kind = #hacc.function_kind<DEVICE>} {
  hivm.hir.debug {debugtype = "print", hex = false, prefix = "gm"} %arg0 : tensor<16x16xf16>
  return
}

// A vector result is stamped VECTOR on UB.
// CHECK-LABEL: func.func @debug_vector
// CHECK: hivm.hir.debug
// CHECK-SAME: memscope = #hivm.address_space<ub>
// CHECK-SAME: tcoretype = #hivm.tcore_type<VECTOR>
func.func @debug_vector(%arg0: tensor<16x16xf16>) attributes {hacc.entry, hacc.function_kind = #hacc.function_kind<DEVICE>} {
  %empty = tensor.empty() : tensor<16x16xf16>
  %add = hivm.hir.vadd ins(%arg0, %arg0 : tensor<16x16xf16>, tensor<16x16xf16>) outs(%empty : tensor<16x16xf16>) -> tensor<16x16xf16>
  hivm.hir.debug {debugtype = "print", hex = false, prefix = "vec"} %add : tensor<16x16xf16>
  return
}
