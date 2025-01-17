##===- Makefile -----------------------------------------------------------===##
# 
# This file licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# 
##===----------------------------------------------------------------------===##

include ../../makefile-common

targetname = testExp

all: build/final.xclbin build/insts.txt

build/dut.cc: bf16_softmax.mlir
	mkdir -p ${@D}
	cd ${@D} &&	aie-opt ../$< -affine-super-vectorize="virtual-vector-size=16 test-fastest-varying=0 vectorize-reductions=true" --convert-vector-to-aievec="aie-target=aieml" -lower-affine | aie-translate -aieml=true --aievec-to-cpp -o ${@F}

build/dut.o: build/dut.cc
	cd ${@D} &&	xchesscc_wrapper ${CHESSCCWRAP2_FLAGS} -I../../../../aie_runtime_lib/AIE2 -c $(<:%=../%) -o ${@F}

build/lut_based_ops.o:
	mkdir -p ${@D}
	cd ${@D} &&	xchesscc_wrapper ${CHESSCCWRAP2_FLAGS} -I. -c ../../../../aie_runtime_lib/AIE2/lut_based_ops.cpp -o ${@F}

build/softmax.o: softmax.cc
	mkdir -p ${@D}
	cd ${@D} && xchesscc_wrapper ${CHESSCCWRAP2_FLAGS} -I. -I../../../../aie_runtime_lib/AIE2 -c $(<:%=../%) -o ${@F}

build/kernels.a: build/softmax.o build/lut_based_ops.o build/dut.o
	ar rvs $@ $+

build/aie.mlir: aie2.py
	mkdir -p ${@D}
	python3 $< > $@

build/final.xclbin: build/aie.mlir build/kernels.a
	mkdir -p ${@D}
	cd ${@D} && aiecc.py --aie-generate-cdo --no-compile-host --xclbin-name=${@F} \
				--aie-generate-ipu --ipu-insts-name=insts.txt $(<:%=../%)

${targetname}.exe: test.cpp
	rm -rf _build
	mkdir -p _build
	cd _build && ${powershell} cmake .. -DTARGET_NAME=${targetname}
	cd _build && ${powershell} cmake --build . --config Release
ifeq "${powershell}" "powershell.exe"
	cp _build/${targetname}.exe $@
else
	cp _build/${targetname} $@ 
endif

run: ${targetname}.exe build/final.xclbin build/insts.txt 
	${powershell} ./$< -x build/final.xclbin -i build/insts.txt -k MLIR_AIE

profile: ${targetname}.exe build/final.xclbin build/insts.txt 
	${powershell} ./$< -x build/final.xclbin -i build/insts.txt -k MLIR_AIE -p results.csv

trace:
	../../utils/parse_eventIR.py --filename trace.txt --mlir build/aie.mlir --colshift 1 > parse_eventIR_vs.json

clean_trace:
	rm -rf tmpTrace trace.txt

clean: clean_trace
	rm -rf build _build ${targetname}.exe 

