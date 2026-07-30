//===- AIECapabilitySubtargetTest.cpp ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2026 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//
//
// Exercises the capability SubtargetFeatures (hasBFP16 / hasAcc2048) directly
// on the backend Subtarget, independent of clang. aie2p carries both
// capabilities; aie2ps shares the 2048-bit accumulator but not bfp16, so a
// capability is a set, not a level.
//
//===----------------------------------------------------------------------===//

#include "TargetInfo/AIETargetInfo.h"
#include "aie2p/AIE2PTargetMachine.h"
#include "aie2ps/AIE2PSTargetMachine.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Target/TargetOptions.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

// Registers the AIE target-info/target/MC callbacks once for this binary, the
// same three init calls llc/clang make before constructing a TargetMachine.
class AIECapabilitySubtargetTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    LLVMInitializeAIETargetInfo();
    LLVMInitializeAIETarget();
    LLVMInitializeAIETargetMC();
  }

  static Function *createDummyFunction(Module &M) {
    LLVMContext &Ctx = M.getContext();
    FunctionType *FTy = FunctionType::get(Type::getVoidTy(Ctx), false);
    return Function::Create(FTy, Function::ExternalLinkage, "f", M);
  }
};

TEST_F(AIECapabilitySubtargetTest, AIE2PHasBFP16AndAcc2048) {
  AIE2PTargetMachine TM(getTheAIE2PTarget(), Triple("aie2p"), "aie2p",
                        /*FS=*/"", TargetOptions(), std::nullopt, std::nullopt,
                        CodeGenOptLevel::Default, /*JIT=*/false);
  LLVMContext Ctx;
  Module M("M", Ctx);
  const AIE2PSubtarget *ST = TM.getSubtargetImpl(*createDummyFunction(M));
  EXPECT_TRUE(ST->hasBFP16());
  EXPECT_TRUE(ST->hasAcc2048());
}

TEST_F(AIECapabilitySubtargetTest, AIE2PBFP16DisabledByFeatureString) {
  AIE2PTargetMachine TM(getTheAIE2PTarget(), Triple("aie2p"), "aie2p",
                        /*FS=*/"-bfp16", TargetOptions(), std::nullopt,
                        std::nullopt, CodeGenOptLevel::Default, /*JIT=*/false);
  LLVMContext Ctx;
  Module M("M", Ctx);
  const AIE2PSubtarget *ST = TM.getSubtargetImpl(*createDummyFunction(M));
  EXPECT_FALSE(ST->hasBFP16());
  // acc2048 is a separate bit in the set; disabling bfp16 does not disable it.
  EXPECT_TRUE(ST->hasAcc2048());
}

TEST_F(AIECapabilitySubtargetTest, AIE2PSHasAcc2048ButNotBFP16) {
  AIE2PSTargetMachine TM(getTheAIE2PSTarget(), Triple("aie2ps"), "aie2ps",
                         /*FS=*/"", TargetOptions(), std::nullopt, std::nullopt,
                         CodeGenOptLevel::Default,
                         /*JIT=*/false);
  LLVMContext Ctx;
  Module M("M", Ctx);
  const AIE2PSSubtarget *ST = TM.getSubtargetImpl(*createDummyFunction(M));
  // The point of a capability SET: aie2ps shares acc2048 with aie2p but does
  // not carry bfp16 (replaced by MX on that revision).
  EXPECT_TRUE(ST->hasAcc2048());
  EXPECT_FALSE(ST->hasBFP16());
}

} // namespace
