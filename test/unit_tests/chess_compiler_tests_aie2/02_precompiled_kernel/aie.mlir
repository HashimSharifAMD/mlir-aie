//===- aie.mlir ------------------------------------------------*- MLIR -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2021 Xilinx Inc.
//
//===----------------------------------------------------------------------===//

// REQUIRES: valid_xchess_license
// RUN: %PYTHON aiecc.py --aiesim --xchesscc --xbridge %VitisSysrootFlag% --host-target=%aieHostTargetTriplet% %s -I%host_runtime_lib%/test_lib/include -L%host_runtime_lib%/test_lib/lib -ltest_lib %S/test.cpp -o test.elf
// RUN: xchesscc_wrapper aie2 +l aie.mlir.prj/core_1_3.bcf %S/kernel.cc -o custom_1_3.elf
// RUN: %run_on_board ./test.elf
// RUN: aie.mlir.prj/aiesim.sh | FileCheck %s

// CHECK: AIE2 ISS
// CHECK: test start.
// CHECK: PASS!

module @test_chess_02_deprecated_precompiled_kernel {
  AIE.device(xcve2802) {
    %tile13 = AIE.tile(1, 3)

    %buf13_0 = AIE.buffer(%tile13) { sym_name = "a" } : memref<256xi32>
    %buf13_1 = AIE.buffer(%tile13) { sym_name = "b" } : memref<256xi32>

    %lock13_3 = AIE.lock(%tile13, 3) { sym_name = "input_lock" }
    %lock13_5 = AIE.lock(%tile13, 5) { sym_name = "output_lock" }

    %core13 = AIE.core(%tile13) { AIE.end } { elf_file = "custom_1_3.elf" }
  }
}