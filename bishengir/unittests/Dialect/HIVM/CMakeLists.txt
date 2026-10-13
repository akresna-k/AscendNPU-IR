set(BISHENGIR_HIVM_TRANSFORM_UNITTEST_SOURCES
  PropagatorCoreTypeTest.cpp
)

# LLVM_INCLUDE_TESTS may be off, in which case add_unittest's llvm_gtest
# libraries are not produced. Compile googletest into this binary then.
if(TARGET llvm_gtest)
  add_bishengir_unittest(BiShengIRHIVMTransformsTests
    ${BISHENGIR_HIVM_TRANSFORM_UNITTEST_SOURCES}
  )
else()
  set(GTEST_ROOT ${LLVM_THIRD_PARTY_DIR}/unittest/googletest)
  set(EXCLUDE_FROM_ALL ON)
  add_llvm_executable(BiShengIRHIVMTransformsTests
    IGNORE_EXTERNALIZE_DEBUGINFO NO_INSTALL_RPATH
    ${BISHENGIR_HIVM_TRANSFORM_UNITTEST_SOURCES}
    PropagatorCoreTypeTestMain.cpp
    ${GTEST_ROOT}/src/gtest-all.cc
  )
  set(EXCLUDE_FROM_ALL OFF)

  target_include_directories(BiShengIRHIVMTransformsTests PRIVATE
    ${GTEST_ROOT}/include
    ${GTEST_ROOT}
  )
  target_compile_definitions(BiShengIRHIVMTransformsTests PRIVATE
    GTEST_HAS_RTTI=0
  )
  target_compile_options(BiShengIRHIVMTransformsTests PRIVATE
    -Wno-suggest-override
    -Wno-covered-switch-default
    -Wno-variadic-macros
  )
  target_link_libraries(BiShengIRHIVMTransformsTests PRIVATE
    LLVMSupport
  )

  set(run_target run-BiShengIRHIVMTransformsTests)
  add_custom_target(${run_target}
    COMMAND $<TARGET_FILE:BiShengIRHIVMTransformsTests>
    DEPENDS BiShengIRHIVMTransformsTests
    COMMENT "Run BiShengIRHIVMTransformsTests unit tests"
    EXCLUDE_FROM_ALL
  )
  set_property(GLOBAL APPEND PROPERTY BISHENGIR_UNITTEST_RUN_TARGETS
               ${run_target})
  add_dependencies(BiShengIRUnitTests BiShengIRHIVMTransformsTests)
endif()

target_link_libraries(BiShengIRHIVMTransformsTests PRIVATE
  BiShengIRHIVMDialect
  BiShengIRHIVMTransforms
  MLIRFuncDialect
  MLIRIR
)

target_include_directories(BiShengIRHIVMTransformsTests PRIVATE
  ${LLVM_THIRD_PARTY_DIR}/unittest/googletest/include
)
