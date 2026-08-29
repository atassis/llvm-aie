//===- event.s --------------------------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2026 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//
// event #0 and #1 spell their immediate inside the AsmString rather than
// carrying an operand, so the opcode has to be chosen from the number.
// event.warning and event.error are separate mnemonics that match normally,
// and are here so the four encodings can be read against each other: they
// agree outside the field the number selects.
//
// RUN: llvm-mc -triple aie2ps %s -o - | FileCheck %s
// RUN: llvm-mc -triple aie2ps %s -filetype=obj -o %t
// RUN: llvm-objdump --triple=aie2ps -d %t | FileCheck %s --check-prefix=OBJ
//
// Only 0 and 1 name an opcode. Anything else keeps the matcher's diagnostic.
// RUN: not llvm-mc -triple aie2ps -filetype=obj --defsym BAD=1 %s -o /dev/null \
// RUN:   2>&1 | FileCheck %s --check-prefix=ERR

	.text
	event	#0
	event	#1
	event.warning
	event.error

.ifdef BAD
	event	#2
.endif

// CHECK: event #0
// CHECK: event #1
// CHECK: event.warning
// CHECK: event.error

// OBJ: 30 00 10 20  	event	#0
// OBJ-NEXT: 30 00 10 22  	event	#1
// OBJ-NEXT: 30 00 10 24  	event.warning
// OBJ-NEXT: 30 00 10 26  	event.error

// ERR: error: invalid operand for instruction
