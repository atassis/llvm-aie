//===---AIEBaseTargetTransformInfo.h - AIEngine generic TTI -*- C++  ----*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2024-2026 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//
//
// This file act as base for AIE's specific information to provide answers to
// certain TTI queries and let child classes to provide even more precise
// answers while letting the target independent and default TTI implementations
// handle the rest.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AIE_AIEBASETARGETTRANSFORMINFO_H
#define LLVM_LIB_TARGET_AIE_AIEBASETARGETTRANSFORMINFO_H

#include "aie1/AIE1Subtarget.h" // For AIEBaseSubTarget
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"

namespace llvm {
/// This is just a bunch of shared methods that can be easily reused.
/// It is foreseen that some of them may be overridden in derived classes used
/// by the actual TTIImpl classes
class AIETTICommon {
public:
  virtual ~AIETTICommon() = default;
  virtual bool isLoweredToCall(const Function *F);
  virtual bool isAllowedInZOL(llvm::Instruction &Instr);
  void adjustUnrollingPreferences(Loop *L, ScalarEvolution &SE,
                                  TTI::UnrollingPreferences &UP,
                                  OptimizationRemarkEmitter *ORE);
  bool isHardwareLoopProfitable(Loop *L, ScalarEvolution &SE,
                                AssumptionCache &AC, TargetLibraryInfo *LibInfo,
                                HardwareLoopInfo &HWLoopInfo);
  bool isProfitableOuterLSR(const Loop &L) const;

  InstructionCost getMemoryOpCost(unsigned Opcode, Type *Src, Align Alignment,
                                  unsigned AddressSpace,
                                  const DataLayout &DL) const;
};

template <typename T> class AIEBaseTTIImpl : public BasicTTIImplBase<T> {
private:
  using BaseT = BasicTTIImplBase<T>;
  using TTI = TargetTransformInfo;
  friend BaseT;
  const AIESubtarget *ST;
  const AIEBaseTargetLowering *TLI;

  const AIESubtarget *getST() const { return ST; }
  const AIEBaseTargetLowering *getTLI() const { return TLI; }
  /// Helper function to access this as a T.
  T *thisT() { return static_cast<T *>(this); }

protected:
  explicit AIEBaseTTIImpl(const TargetMachine *TM, const DataLayout &DL,
                          const AIESubtarget *Subtarget)
      : BaseT(TM, DL), ST(Subtarget), TLI(Subtarget->getTargetLowering()) {}
  virtual ~AIEBaseTTIImpl() = default;

public:
  //===--------------------------------------------------------------------===//
  // Cost Model
  //===--------------------------------------------------------------------===//

  int getIntImmCost(const APInt &Imm, Type *Ty, TTI::TargetCostKind CostKind) {
    // TODO Handle Target Specific constant cost
    //  Larger constants require an add.
    return TTI::TCC_Basic;
  }
  InstructionCost getMaskedMemoryOpCost(
      unsigned Opcode, Type *Src, Align Alignment, unsigned AddressSpace,
      TTI::TargetCostKind CostKind = TTI::TCK_RecipThroughput) const {
    // Default cost is 32.  We can do better than that, but what is the real
    // cost?
    return TTI::TCC_Basic;
  }

  //===--------------------------------------------------------------------===//
  // Loop Optimization
  //===--------------------------------------------------------------------===//

  void adjustUnrollingPreferences(Loop *L, ScalarEvolution &SE,
                                  TTI::UnrollingPreferences &UP,
                                  OptimizationRemarkEmitter *ORE);
  bool isHardwareLoopProfitable(Loop *L, ScalarEvolution &SE,
                                AssumptionCache &AC, TargetLibraryInfo *LibInfo,
                                HardwareLoopInfo &HWLoopInfo);

  //===--------------------------------------------------------------------===//
  // Vectorization
  //===--------------------------------------------------------------------===//

  // We define a store vector factor of  4 for 8-bit and 2 for 16-bit. This
  // allows combining 2 16-bit stores or 4 8-bit stores into a single 32-bit
  // vector store. This is deemed beneficial because of the LMS nature of
  // part-word stores which require more cycles to complete and need to stay
  // clear of other memory instructions.
  unsigned getStoreVectorFactor(unsigned VF, unsigned StoreSizeInBits,
                                unsigned ChainSizeInBytes,
                                VectorType *VecTy) const {
    // The cases of interest are 8 and 16-bit only.
    return (StoreSizeInBits == 8) ? 4 : (StoreSizeInBits == 16) ? 2 : 1;
  }

  // This load vector factor of 1 basically prevents load vectorization which is
  // deemed too expensive. The reason being that we have to shift out each
  // element after the block load and perform a sign extension for each, whereas
  // the scalarized approach results in only 4 part-word loads with implicit
  // sign extension.
  unsigned getLoadVectorFactor(unsigned VF, unsigned LoadSizeInBits,
                               unsigned ChainSizeInBytes,
                               VectorType *VecTy) const {
    // Block load vectorization, it is costly to extract elements from vectors.
    return 1;
  }

  bool isLegalToVectorizeStoreChain(unsigned ChainSizeInBytes, Align Alignment,
                                    unsigned AddrSpace) const {
    // Start from 4 byte sequences, to reach word stores. Alignment is
    // considered by default by the pass.
    // Default return of allowsMisalignedMemoryAccesses is false.
    return ChainSizeInBytes >= 4;
  }

  //===--------------------------------------------------------------------===//
  // Loop Strength Reduction (LSR)
  //
  // AIE has 20-bit pointers but 32-bit integers, and post-increment load/store
  // instructions (VLD_pstm, VST_pstm). These hooks enable LSR to generate
  // pointer recurrences that the backend can combine with post-increment ops.
  //===--------------------------------------------------------------------===//

  /// Check if type is an integer matching the target's index size (e.g., i20).
  /// Note: uses address space 0; all AIE address spaces share the same index
  /// width.
  static bool isIndexSizedInteger(Type *Ty, const DataLayout &DL) {
    return Ty->isIntegerTy() &&
           Ty->getIntegerBitWidth() == DL.getIndexSizeInBits(0);
  }

  /// Collect all GEP users of \p Trunc that use it as an index operand (not
  /// the pointer operand). Returns false if any use is not a GEP index.
  static bool collectGEPIndices(const TruncInst *Trunc,
                                SmallVectorImpl<GetElementPtrInst *> &GEPs) {
    for (const Use &U : Trunc->uses()) {
      auto *GEP = dyn_cast<GetElementPtrInst>(U.getUser());
      if (!GEP || U.getOperandNo() == 0)
        return false;
      GEPs.push_back(GEP);
    }
    return true;
  }

  /// Prefer pointer-based recurrences over scalar offset + base formulations.
  TTI::AddressingModeKind
  getPreferredAddressingMode(const Loop *L, ScalarEvolution *SE) const {
    return TTI::AMK_PostIndexed;
  }

  /// Enable post-increment addressing for index-sized integers (i20).
  bool isIndexedLoadLegal(TTI::MemIndexedMode Mode, Type *Ty,
                          const DataLayout &DL) const {
    return Mode == TTI::MIM_PostInc && isIndexSizedInteger(Ty, DL);
  }

  bool isIndexedStoreLegal(TTI::MemIndexedMode Mode, Type *Ty,
                           const DataLayout &DL) const {
    return Mode == TTI::MIM_PostInc && isIndexSizedInteger(Ty, DL);
  }

  /// Look through truncs to index size that feed GEP indices.
  ///
  /// Array indexing generates: %trunc = trunc i32 %idx to i20
  /// Without this hook, IVUsers() stops at the trunc (i20 not legal).
  /// With this hook, IVUsers() continues to the GEP, collecting pointer SCEVs.
  bool shouldIVUsersLookThroughInst(
      Instruction *I,
      SmallVectorImpl<GetElementPtrInst *> &GEPsToProcess) const {
    auto *Trunc = dyn_cast<TruncInst>(I);
    if (!Trunc)
      return false;

    if (!Trunc->getType()->isIntegerTy())
      return false;

    const DataLayout &DL = Trunc->getModule()->getDataLayout();
    const unsigned TruncWidth = Trunc->getType()->getIntegerBitWidth();
    // All AIE address spaces share the same index width; use address space 0.
    const unsigned IndexWidth = DL.getIndexSizeInBits(/*AS=*/0);

    if (TruncWidth != IndexWidth || DL.isLegalInteger(TruncWidth))
      return false;

    return collectGEPIndices(Trunc, GEPsToProcess);
  }

  /// Prioritize fewer loop-body adds over fewer recurrences.
  /// For VLIW, extra adds hurt II while extra PHIs execute in parallel.
  bool isLSRCostLess(const TTI::LSRCost &C1, const TTI::LSRCost &C2) const {
    return std::tie(C1.NumRegs, C1.Insns, C1.NumBaseAdds, C1.AddRecCost,
                    C1.NumIVMuls, C1.ScaleCost, C1.ImmCost, C1.SetupCost) <
           std::tie(C2.NumRegs, C2.Insns, C2.NumBaseAdds, C2.AddRecCost,
                    C2.NumIVMuls, C2.ScaleCost, C2.ImmCost, C2.SetupCost);
  }

  /// Extend valid IV user types to include index-sized integers (i20).
  bool isValidIVUserType(Type *Ty) const {
    return BaseT::isValidIVUserType(Ty) ||
           isIndexSizedInteger(Ty, BaseT::getDataLayout());
  }
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_AIE_AIEBASETARGETTRANSFORMINFO_H
