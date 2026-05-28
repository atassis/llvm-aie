//===- AIEOuterLoopEpilogRename.cpp - Outer-loop epilog WAR break -*- C++ -*-=//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2026 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//
//
// Breaks a write-after-read hazard on the steady-state epilog block produced
// by AIEOuterLoopPipeliner. The epilog's first vlda chain typically defines
// sub-registers of an accumulator (e.g. $bmll3 ⊂ $dm3) that the predecessor
// inner loop also writes; after the post-pipeliner schedules the drain, the
// WAR pushes the chain head several bundles late.
//
// The pass operates by splitting the chain-head's virtual register: a fresh
// vreg is created, the chain-head sub-register defs are redirected to it, and
// a glue COPY is inserted before the terminator so the back-edge target still
// sees the original vreg. The fresh vreg is then pre-pinned (via
// LiveRegMatrix::assign) to a physreg chosen to *avoid* every physreg the
// predecessor loop writes — greedy's cost model cannot see the
// postpipeliner-level WAR cost, so the pin is essential.
//
//===----------------------------------------------------------------------===//

#include "AIE.h"
#include "Utils/AIELoopUtils.h"

#include "llvm/CodeGen/LiveDebugVariables.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "aie-outer-loop-epilog-rename"

namespace {

class AIEOuterLoopEpilogRename : public MachineFunctionPass {
  MachineRegisterInfo *MRI = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  const TargetInstrInfo *TII = nullptr;
  VirtRegMap *VRM = nullptr;
  LiveRegMatrix *LRM = nullptr;
  LiveIntervals *LIS = nullptr;

  bool tryRenameEpilog(MachineBasicBlock &MBB);

  /// Reg-units written by any def in \p Pred, resolved via VRM. Liveins are
  /// not a substitute: the WAR is a post-pipeliner scheduling hazard against
  /// recent writes, which includes accumulator slices dead at the back-edge.
  BitVector collectPredRegUnitWrites(const MachineBasicBlock &Pred) const;

  /// First non-debug load in \p MBB whose VRM-assigned physreg (incl.
  /// sub-reg-index resolution) shares any reg-unit with \p PredUnits.
  MachineInstr *findLoadChainHead(MachineBasicBlock &MBB,
                                  const BitVector &PredUnits) const;

  /// Rename the chain-head vreg defined at \p Head to a physreg that avoids
  /// \p PredUnits and the original physreg. Performs the SSA-level split:
  /// allocate fresh vreg, rewrite operands in \p MBB, insert glue COPY,
  /// refresh live intervals, and pin via LRM. Returns true on success.
  bool renameChainHead(MachineBasicBlock &MBB, MachineInstr &Head,
                       const BitVector &PredUnits);

  /// Allocate a fresh vreg in the same class as \p OldVReg and grow VRM.
  Register createSplitVReg(Register OldVReg);

  /// Replace every operand of \p OldVReg at/after \p ChainHead in \p MBB
  /// with \p NewVReg. (MRI's replaceRegWith is whole-function; this rewrite
  /// must stay scoped to the MBB suffix starting at the chain head, so the
  /// rewrite is open-coded.)
  void replaceRegisterFromChainHead(MachineBasicBlock &MBB,
                                    MachineInstr &ChainHead, Register OldVReg,
                                    Register NewVReg) const;

  /// Build "OldVReg = COPY NewVReg" at \p InsertPt in \p MBB and register it
  /// with SlotIndexes.
  void insertGlueCopy(MachineBasicBlock &MBB,
                      MachineBasicBlock::iterator InsertPt, Register OldVReg,
                      Register NewVReg) const;

  /// Recompute live intervals for \p OldVReg and \p NewVReg after the split.
  /// Clears stale `dead` flags carried over by setReg before the glue COPY
  /// turns the previously-dead defs into real uses.
  void refreshIntervals(Register OldVReg, Register NewVReg);

  /// Pick a non-CSR physreg in \p RC that avoids \p BlockedUnits and is free
  /// over [\p Start, \p End), or NoRegister if none survives.
  MCPhysReg pickRenamePhysReg(const TargetRegisterClass &RC,
                              const BitVector &BlockedUnits, SlotIndex Start,
                              SlotIndex End) const;

public:
  static char ID;
  AIEOuterLoopEpilogRename() : MachineFunctionPass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<VirtRegMapWrapperLegacy>();
    AU.addPreserved<VirtRegMapWrapperLegacy>();
    AU.addRequired<SlotIndexesWrapperPass>();
    AU.addPreserved<SlotIndexesWrapperPass>();
    AU.addRequired<LiveDebugVariablesWrapperLegacy>();
    AU.addPreserved<LiveDebugVariablesWrapperLegacy>();
    AU.addRequired<LiveStacksWrapperLegacy>();
    AU.addPreserved<LiveStacksWrapperLegacy>();
    AU.addRequired<LiveIntervalsWrapperPass>();
    AU.addPreserved<LiveIntervalsWrapperPass>();
    AU.addRequired<LiveRegMatrixWrapperLegacy>();
    AU.addPreserved<LiveRegMatrixWrapperLegacy>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // end anonymous namespace

/// Set the bits for the reg-units of \p Phys in \p Out. \p Out is expected to
/// be sized TRI.getNumRegUnits(); bits are *added*, never cleared.
static void addRegUnits(const TargetRegisterInfo &TRI, MCRegister Phys,
                        BitVector &Out) {
  for (MCRegUnit RU : TRI.regunits(Phys))
    Out.set(RU);
}

bool AIEOuterLoopEpilogRename::runOnMachineFunction(MachineFunction &MF) {
  MRI = &MF.getRegInfo();
  TRI = MRI->getTargetRegisterInfo();
  TII = MF.getSubtarget().getInstrInfo();
  VRM = &getAnalysis<VirtRegMapWrapperLegacy>().getVRM();
  LRM = &getAnalysis<LiveRegMatrixWrapperLegacy>().getLRM();
  LIS = &getAnalysis<LiveIntervalsWrapperPass>().getLIS();

  LLVM_DEBUG(dbgs() << "*** AIE OuterLoop Epilog WAR: " << MF.getName()
                    << " ***\n");

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF)
    if (AIELoopUtils::isOuterLoopEpilog(MBB))
      Changed |= tryRenameEpilog(MBB);
  return Changed;
}

BitVector AIEOuterLoopEpilogRename::collectPredRegUnitWrites(
    const MachineBasicBlock &Pred) const {
  BitVector Units(TRI->getNumRegUnits());
  for (const MachineInstr &MI : Pred) {
    for (const MachineOperand &MO : MI.all_defs()) {
      const Register R = MO.getReg();

      // Resolve to the concrete physreg the def lands in
      MCRegister Phys;
      if (R.isVirtual()) {
        if (!VRM->hasPhys(R))
          continue;
        Phys = VRM->getPhys(R);
      } else if (R.isPhysical()) {
        Phys = R.asMCReg();
      }
      if (!Phys)
        continue;

      // Accumulate this def's reg-units into the running set.
      addRegUnits(*TRI, Phys, Units);
    }
  }
  return Units;
}

MachineInstr *
AIEOuterLoopEpilogRename::findLoadChainHead(MachineBasicBlock &MBB,
                                            const BitVector &PredUnits) const {
  BitVector Scratch(TRI->getNumRegUnits());
  for (MachineInstr &MI : MBB) {
    // Debug instructions carry no real defs and never participate in WARs.
    if (MI.isDebugInstr())
      continue;

    // The outer-loop pipeliner rotates loads from the prologue into the
    // epilog: those are the loads whose schedule we are trying to free up.
    // Restricting to loads also skips pointer-update pseudos (PADD_*) that
    // can legitimately write a physreg the predecessor also writes.
    if (!MI.mayLoad())
      continue;

    // AIE load instructions place the loaded value in operand 0.
    const MachineOperand &MO = MI.getOperand(0);
    assert(MO.isReg() && MO.isDef() &&
           "AIE load expected to define a register in operand 0");

    // Need a VRM-assigned virtual register to know the concrete physreg the
    // post-RA hazard will form against.
    const Register Def = MO.getReg();
    if (!Def.isVirtual() || !VRM->hasPhys(Def))
      continue;

    // Resolve the destination operand to its concrete (sub-)physreg.
    MCRegister Phys = VRM->getPhys(Def);
    if (unsigned SubIdx = MO.getSubReg())
      Phys = TRI->getSubReg(Phys, SubIdx);
    if (!Phys)
      continue;

    // A WAR exists iff this load's destination reg-units intersect anything
    // the predecessor wrote.
    Scratch.reset();
    addRegUnits(*TRI, Phys, Scratch);
    if (Scratch.anyCommon(PredUnits))
      return &MI;
  }
  return nullptr;
}

Register AIEOuterLoopEpilogRename::createSplitVReg(Register OldVReg) {
  const Register NewVReg =
      MRI->createVirtualRegister(MRI->getRegClass(OldVReg));
  VRM->grow();
  return NewVReg;
}

void AIEOuterLoopEpilogRename::replaceRegisterFromChainHead(
    MachineBasicBlock &MBB, MachineInstr &ChainHead, Register OldVReg,
    Register NewVReg) const {
  for (MachineBasicBlock::iterator It = ChainHead.getIterator(), E = MBB.end();
       It != E; ++It)
    for (MachineOperand &MO : It->operands())
      if (MO.isReg() && MO.getReg() == OldVReg)
        MO.setReg(NewVReg);
}

void AIEOuterLoopEpilogRename::insertGlueCopy(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator InsertPt,
    Register OldVReg, Register NewVReg) const {
  MachineInstr *Glue =
      BuildMI(MBB, InsertPt, DebugLoc(), TII->get(TargetOpcode::COPY), OldVReg)
          .addReg(NewVReg)
          .getInstr();
  LIS->InsertMachineInstrInMaps(*Glue);
}

void AIEOuterLoopEpilogRename::refreshIntervals(Register OldVReg,
                                                Register NewVReg) {
  for (MachineOperand &Def : MRI->def_operands(OldVReg))
    Def.setIsDead(false);
  for (MachineOperand &Def : MRI->def_operands(NewVReg))
    Def.setIsDead(false);
  LIS->removeInterval(OldVReg);
  LIS->createAndComputeVirtRegInterval(OldVReg);
  LIS->createAndComputeVirtRegInterval(NewVReg);
}

MCPhysReg AIEOuterLoopEpilogRename::pickRenamePhysReg(
    const TargetRegisterClass &RC, const BitVector &BlockedUnits,
    SlotIndex Start, SlotIndex End) const {
  // Precompute the CSR set so the per-candidate test below is a single lookup.
  BitVector CSRs(TRI->getNumRegs());
  for (const MCPhysReg *CSR = MRI->getCalleeSavedRegs(); CSR && *CSR; ++CSR)
    CSRs.set(*CSR);

  BitVector Scratch(TRI->getNumRegUnits());
  for (MCPhysReg P : RC.getRegisters()) {
    // Skip CSRs — renaming into one would force a save/restore.
    if (CSRs.test(P))
      continue;

    // Skip anything that overlaps the predecessor's writes or the original
    // physreg (the latter would be a no-op rename).
    Scratch.reset();
    addRegUnits(*TRI, P, Scratch);
    if (Scratch.anyCommon(BlockedUnits))
      continue;

    // Finally check it is actually free over the post-split live range.
    if (LRM->checkInterference(Start, End, P))
      continue;

    return P;
  }
  return MCRegister::NoRegister;
}

bool AIEOuterLoopEpilogRename::renameChainHead(MachineBasicBlock &MBB,
                                               MachineInstr &Head,
                                               const BitVector &PredUnits) {
  const Register OldVReg = Head.getOperand(0).getReg();
  assert(OldVReg.isVirtual() && VRM->hasPhys(OldVReg) &&
         "chain head was selected via VRM lookup on a virtual reg");
  const MCPhysReg OrigPhys = VRM->getPhys(OldVReg);
  const TargetRegisterClass *RC = MRI->getRegClass(OldVReg);

  LLVM_DEBUG(dbgs() << "  chain head: " << Head
                    << "  OldVReg=" << printReg(OldVReg, TRI)
                    << " OrigPhys=" << printReg(OrigPhys, TRI) << "\n");

  // Block both the predecessor's writes and the original physreg (avoid a
  // no-op rename).
  BitVector Blocked = PredUnits;
  addRegUnits(*TRI, OrigPhys, Blocked);

  // Reuse GlueInsertPt for both the interference query and the COPY below so
  // the queried range and the emitted COPY cannot drift apart. RangeEnd is
  // the slot *at* GlueInsertPt: BuildMI inserts before it, so the COPY's
  // later-assigned slot falls strictly before this upper bound.
  const MachineBasicBlock::iterator GlueInsertPt = MBB.getFirstTerminator();
  const SlotIndex RangeStart = LIS->getInstructionIndex(Head);
  const SlotIndex RangeEnd = GlueInsertPt == MBB.end()
                                 ? LIS->getMBBEndIdx(&MBB)
                                 : LIS->getInstructionIndex(*GlueInsertPt);
  const MCPhysReg RenamePhys =
      pickRenamePhysReg(*RC, Blocked, RangeStart, RangeEnd);
  if (!RenamePhys) {
    LLVM_DEBUG(dbgs() << "  no rename target available\n");
    return false;
  }

  const Register NewVReg = createSplitVReg(OldVReg);
  replaceRegisterFromChainHead(MBB, Head, OldVReg, NewVReg);
  insertGlueCopy(MBB, GlueInsertPt, OldVReg, NewVReg);
  refreshIntervals(OldVReg, NewVReg);
  LRM->assign(LIS->getInterval(NewVReg), RenamePhys);
  LLVM_DEBUG(dbgs() << "  pinned " << printReg(NewVReg, TRI) << " to "
                    << printReg(RenamePhys, TRI) << "\n");
  return true;
}

bool AIEOuterLoopEpilogRename::tryRenameEpilog(MachineBasicBlock &MBB) {
  LLVM_DEBUG(dbgs() << "Try epilog rename on " << MBB.getFullName() << "\n");

  // Step 1: get the single-MBB inner-loop predecessor.
  MachineBasicBlock *Pred = AIELoopUtils::getLoopPredecessor(MBB);
  if (!Pred) {
    LLVM_DEBUG(dbgs() << "  no single-MBB-loop predecessor\n");
    return false;
  }

  // Step 2: collect every reg-unit the predecessor writes.
  const BitVector PredUnits = collectPredRegUnitWrites(*Pred);

  // Step 3: find the rotated-in load whose destination collides with that set.
  MachineInstr *Head = findLoadChainHead(MBB, PredUnits);
  if (!Head) {
    LLVM_DEBUG(dbgs() << "  no WAR-blocked chain head\n");
    return false;
  }

  // Step 4: rename the chain-head vreg to a non-blocked physreg.
  return renameChainHead(MBB, *Head, PredUnits);
}

char AIEOuterLoopEpilogRename::ID = 0;
INITIALIZE_PASS(AIEOuterLoopEpilogRename, DEBUG_TYPE,
                "AIE outer-loop epilog rename", false, false)

llvm::FunctionPass *llvm::createAIEOuterLoopEpilogRename() {
  return new AIEOuterLoopEpilogRename();
}
