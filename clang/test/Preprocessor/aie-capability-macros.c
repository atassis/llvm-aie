//===- aie-capability-macros.c ----------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// The AIE capability macros are derived from SubtargetFeatures, so a kernel can
// query a capability instead of hard-coding the generation with an arch macro.
// REQUIRES: aie-registered-target

// aie2p enables bfp16 by default, so the capability macro is defined.
// RUN: %clang_cc1 -E -dM -triple=aie2p < /dev/null | \
// RUN:     FileCheck -match-full-lines -check-prefix BFP16 %s

// The same target with the feature turned off does not define the macro,
// showing the macro tracks the capability rather than the target name.
// RUN: %clang_cc1 -E -dM -triple=aie2p -target-feature -bfp16 < /dev/null | \
// RUN:     FileCheck -check-prefix NO-BFP16 %s

// A different revision (aie2ps) does not carry bfp16, but it does share the
// 2048-bit accumulator with aie2p. This is the point of a capability set: it
// is neither a monotonic level (bfp16 is aie2p-only) nor fully disjoint
// (acc2048 is shared), so it has to be queried as a set.
// RUN: %clang_cc1 -E -dM -triple=aie2ps < /dev/null | \
// RUN:     FileCheck -match-full-lines -check-prefixes NO-BFP16,ACC2048 %s

// BFP16: #define __AIE_HAS_BFP16__ 1
// NO-BFP16-NOT: __AIE_HAS_BFP16__
// ACC2048: #define __AIE_HAS_ACC2048__ 1
