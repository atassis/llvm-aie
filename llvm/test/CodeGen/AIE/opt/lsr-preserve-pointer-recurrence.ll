;
; This file is licensed under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; (c) Copyright 2026 Advanced Micro Devices, Inc. or its affiliates

; RUN: opt -mtriple=aie2p -passes=loop-reduce -S %s | FileCheck %s

; This test verifies that LSR preserves pointer recurrences on AIE targets.
; AIE processors support post-increment addressing modes (VLD_pstm, VST_pstm)
; that fold pointer updates into memory operations for free. LSR should NOT
; rewrite pointer PHIs to scalar offset + base formulas, as this would
; prevent post-increment combining and introduce extra PADD instructions.
;
; Specifically, this test checks that:
; 1. Pointer PHIs are preserved (not rewritten to %scevgep or similar)
; 2. GEP chains retain their original structure with inbounds
; 3. addrspacecast operations don't trigger unwanted IV chain processing

target datalayout = "e-m:e-p:20:32-i1:8:32-i8:8:32-i16:16:32-i32:32:32-f32:32:32-i64:32-f64:32-a:0:32-n32"
target triple = "aie2p"

; Test: Multiple pointer recurrences with variable stride through addrspacecast
; The pointer PHIs should be preserved as-is, not rewritten by LSR.
;
; CHECK-LABEL: define void @multi_pointer_addrspacecast
; CHECK:       loop:
; CHECK:         %p_ifm = phi ptr [ %ifm, %entry ], [ %next_ifm, %loop ]
; CHECK:         %p_ofm = phi ptr [ %ofm, %entry ], [ %next_ofm, %loop ]
; CHECK-NOT:     scevgep
; CHECK:         %next_ifm = getelementptr inbounds i8, ptr %p_ifm, i20 %stride
; CHECK:         %next_ofm = getelementptr inbounds i8, ptr %p_ofm, i20 %stride
; CHECK:       exit:
define void @multi_pointer_addrspacecast(ptr %ifm, ptr %ofm, i20 %stride, i32 %n) {
entry:
  br label %loop

loop:
  %p_ifm = phi ptr [ %ifm, %entry ], [ %next_ifm, %loop ]
  %p_ofm = phi ptr [ %ofm, %entry ], [ %next_ofm, %loop ]
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]

  %ifm_as = addrspacecast ptr %p_ifm to ptr addrspace(5)
  %ofm_as = addrspacecast ptr %p_ofm to ptr addrspace(7)

  %val = load <16 x i32>, ptr addrspace(5) %ifm_as, align 64
  store <16 x i32> %val, ptr addrspace(7) %ofm_as, align 64

  %next_ifm = getelementptr inbounds i8, ptr %p_ifm, i20 %stride
  %next_ofm = getelementptr inbounds i8, ptr %p_ofm, i20 %stride

  %i.next = add i32 %i, 1
  %cond = icmp slt i32 %i.next, %n
  br i1 %cond, label %loop, label %exit

exit:
  ret void
}

; Test: GEP chain within a loop (multiple loads at offsets)
; This pattern can form IV chains in LSR. LSR should NOT rewrite these.
;
; CHECK-LABEL: define void @gep_chain_pattern
; CHECK:       loop:
; CHECK:         %p = phi ptr [ %base, %entry ], [ %p3, %loop ]
; CHECK-NOT:     scevgep
; CHECK:         %p1 = getelementptr inbounds i8, ptr %p, i20 %stride
; CHECK:         %p2 = getelementptr inbounds i8, ptr %p1, i20 %stride
; CHECK:         %p3 = getelementptr inbounds i8, ptr %p2, i20 %stride
; CHECK:       exit:
define void @gep_chain_pattern(ptr %base, i20 %stride, i32 %n) {
entry:
  br label %loop

loop:
  %p = phi ptr [ %base, %entry ], [ %p3, %loop ]
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]

  %as0 = addrspacecast ptr %p to ptr addrspace(5)
  %v0 = load <16 x i32>, ptr addrspace(5) %as0, align 64

  %p1 = getelementptr inbounds i8, ptr %p, i20 %stride
  %as1 = addrspacecast ptr %p1 to ptr addrspace(5)
  %v1 = load <16 x i32>, ptr addrspace(5) %as1, align 64

  %p2 = getelementptr inbounds i8, ptr %p1, i20 %stride
  %as2 = addrspacecast ptr %p2 to ptr addrspace(5)
  %v2 = load <16 x i32>, ptr addrspace(5) %as2, align 64

  %p3 = getelementptr inbounds i8, ptr %p2, i20 %stride

  call void @consume(<16 x i32> %v0)
  call void @consume(<16 x i32> %v1)
  call void @consume(<16 x i32> %v2)

  %i.next = add i32 %i, 1
  %cond = icmp slt i32 %i.next, %n
  br i1 %cond, label %loop, label %exit

exit:
  ret void
}

declare void @consume(<16 x i32>)

; Test: i20 scalar recurrences should still be handled by LSR
; This ensures the IVUsers change doesn't break i20 integer optimization
;
; CHECK-LABEL: define i20 @i20_scalar_recurrence
; CHECK:       loop:
; CHECK:         %i = phi i20
; CHECK:         %sum = phi i20
define i20 @i20_scalar_recurrence(i20 %n, i20 %step) {
entry:
  br label %loop

loop:
  %i = phi i20 [ 0, %entry ], [ %i.next, %loop ]
  %sum = phi i20 [ 0, %entry ], [ %sum.next, %loop ]
  
  %sum.next = add i20 %sum, %i
  %i.next = add i20 %i, %step
  
  %cond = icmp slt i20 %i.next, %n
  br i1 %cond, label %loop, label %exit

exit:
  ret i20 %sum.next
}
