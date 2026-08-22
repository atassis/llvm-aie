;
; This file is licensed under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; (c) Copyright 2026 Advanced Micro Devices, Inc. or its affiliates
; RUN: not --crash llc -mtriple=aie2p -enable-post-misched=false %s -o /dev/null 2>&1 | FileCheck %s

; The post-RA machine scheduler is what gives a multi-slot pseudo its concrete
; opcode, so disabling it leaves one for the bundle code to read a slot from.

; CHECK: LLVM ERROR: AIE requires post-RA scheduling

define void @f(ptr %a, i32 %n) {
entry:
  %cmp = icmp sgt i32 %n, 0
  br i1 %cmp, label %body, label %exit

body:
  %i = phi i32 [ 0, %entry ], [ %inc, %body ]
  %gep = getelementptr inbounds i32, ptr %a, i32 %i
  %ld = load i32, ptr %gep, align 4
  %add = add nsw i32 %ld, 1
  store i32 %add, ptr %gep, align 4
  %inc = add nuw nsw i32 %i, 1
  %done = icmp eq i32 %inc, %n
  br i1 %done, label %exit, label %body

exit:
  ret void
}
