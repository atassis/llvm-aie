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

  /// Check that all uses of Trunc are GEP index operands (not pointer operand).
  static bool allUsesAreGEPIndices(const TruncInst *Trunc) {
    for (const Use &U : Trunc->uses()) {
      const auto *GEP = dyn_cast<GetElementPtrInst>(U.getUser());
      const bool IsGEPIndexUse = GEP && U.getOperandNo() != 0;
      if (!IsGEPIndexUse)
        return false;
    }
    return true;
  }

protected:
  explicit AIEBaseTTIImpl(const TargetMachine *TM, const DataLayout &DL,
                          const AIESubtarget *Subtarget)
      : BaseT(TM, DL), ST(Subtarget), TLI(Subtarget->getTargetLowering()) {}
  virtual ~AIEBaseTTIImpl() = default;

public:
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
  void adjustUnrollingPreferences(Loop *L, ScalarEvolution &SE,
                                  TTI::UnrollingPreferences &UP,
                                  OptimizationRemarkEmitter *ORE);
  bool isHardwareLoopProfitable(Loop *L, ScalarEvolution &SE,
                                AssumptionCache &AC, TargetLibraryInfo *LibInfo,
                                HardwareLoopInfo &HWLoopInfo);

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

  /// AIE has post-increment load/store instructions (VLD_pstm, VST_pstm) that
  /// fold pointer updates into memory operations for free. This tells LSR to
  /// prefer pointer-based recurrences over scalar offset + base formulations,
  /// which would require explicit PADD instructions.
  TTI::AddressingModeKind
  getPreferredAddressingMode(const Loop *L, ScalarEvolution *SE) const {
    return TTI::AMK_PostIndexed;
  }

  /// Override to enable post-increment load optimization for AIE.
  ///
  /// AIE has VLD_pstm instructions that combine load + pointer update into a
  /// single operation. LSR uses this hook in mayUsePostIncMode() to decide
  /// whether to preserve recurrence structures. We return true for:
  /// - Pointer types: enables {base,+,stride} recurrences for pointer PHIs
  /// - i20 integers: enables index recurrences that feed GEP indices
  bool isIndexedLoadLegal(TTI::MemIndexedMode Mode, Type *Ty,
                          const DataLayout &DL) const {
    if (Mode != TTI::MIM_PostInc)
      return false;

    const bool IsPointer = Ty->isPointerTy();
    const bool IsIndexSizedInt =
        Ty->isIntegerTy() &&
        Ty->getIntegerBitWidth() == DL.getIndexSizeInBits(0);

    return IsPointer || IsIndexSizedInt;
  }

  /// Override to enable post-increment store optimization for AIE.
  /// See isIndexedLoadLegal for rationale. AIE has VST_pstm instructions.
  bool isIndexedStoreLegal(TTI::MemIndexedMode Mode, Type *Ty,
                           const DataLayout &DL) const {
    if (Mode != TTI::MIM_PostInc)
      return false;

    const bool IsPointer = Ty->isPointerTy();
    const bool IsIndexSizedInt =
        Ty->isIntegerTy() &&
        Ty->getIntegerBitWidth() == DL.getIndexSizeInBits(0);

    return IsPointer || IsIndexSizedInt;
  }

  /// Override to look through truncs to index size that feed GEP indices.
  ///
  /// AIE has 20-bit pointers but 32-bit integers. Array indexing generates:
  ///   %idx = shl i32 %i, 2
  ///   %trunc = trunc i32 %idx to i20
  ///   %ptr = getelementptr ..., i20 %trunc
  ///
  /// Without this hook, IVUsers stops at the trunc (i20 not legal), collecting
  /// integer SCEVs. With this hook, IVUsers continues to the GEP, collecting
  /// pointer SCEVs that enable post-increment addressing.
  bool shouldIVUsersLookThroughInst(
      Instruction *I,
      SmallVectorImpl<GetElementPtrInst *> &GEPsToProcess) const {
    auto *Trunc = dyn_cast<TruncInst>(I);
    if (!Trunc)
      return false;

    const DataLayout &DL = Trunc->getModule()->getDataLayout();
    const unsigned TruncWidth = Trunc->getType()->getIntegerBitWidth();
    const unsigned IndexWidth = DL.getIndexSizeInBits(0);

    const bool IsTruncToIndexSize = (TruncWidth == IndexWidth);
    const bool IsNonLegalIndexSize = !DL.isLegalInteger(TruncWidth);
    if (!IsTruncToIndexSize || !IsNonLegalIndexSize)
      return false;

    if (!allUsesAreGEPIndices(Trunc))
      return false;

    // Collect all GEP users for processing
    for (User *U : Trunc->users())
      GEPsToProcess.push_back(cast<GetElementPtrInst>(U));

    return true;
  }

  /// Override to preserve recurrences for i20 and pointer types in LSR.
  ///
  /// LSR's mayUsePostIncMode uses this to decide if Basic uses should preserve
  /// recurrence structure. For AIE, we want this for i20 (GEP indices) and
  /// pointers, enabling post-increment addressing patterns.
  bool shouldLSRPreserveBasicRecurrence(Type *Ty) const {
    const DataLayout &DL = BaseT::getDataLayout();
    const unsigned IndexWidth = DL.getIndexSizeInBits(0);

    const bool IsPointer = Ty->isPointerTy();
    const bool IsIndexSizedInt =
        Ty->isIntegerTy() && Ty->getIntegerBitWidth() == IndexWidth;

    return IsPointer || IsIndexSizedInt;
  }

  /// Allow index-sized integers and pointers as valid IV user types.
  bool isValidIVUserType(Type *Ty) const {
    const DataLayout &DL = BaseT::getDataLayout();
    const uint64_t Width = DL.getTypeSizeInBits(Ty);
    const unsigned IndexWidth = DL.getIndexSizeInBits(0);

    const bool IsLegalInteger =
        Ty->isIntegerTy() && Width <= 64 && DL.isLegalInteger(Width);
    const bool IsIndexSizedInteger = Ty->isIntegerTy() && Width == IndexWidth;
    const bool IsIndexSizedPointer = Ty->isPointerTy() && Width == IndexWidth;

    return IsLegalInteger || IsIndexSizedInteger || IsIndexSizedPointer;
  }
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_AIE_AIEBASETARGETTRANSFORMINFO_H
