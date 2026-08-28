#!/usr/bin/env python3
"""Stage 2 of an AIE2P differential-harness slice: given a Stage-1 manifest
(extract_opcode.manifest) for a CONTROL-family opcode -- scalar ALU / LDA / PADDS,
the family named as the harness's control in docs/kb/aie2p-oracle-is-the-gap.md --
emit the load-inputs -> run-instruction -> spill-outputs assembly a differential
run needs.

Scope, stated rather than silently assumed: this only knows how to seed a
register-class input through MOVXM (reaches the mMvSclDst family, which includes
eR) and to spill a register-class output through ST_dms_sts_idx_imm (reaches the
mSclSt family, also including eR). An operand class outside both sets raises
NotImplementedError rather than emitting something plausible-looking and wrong --
the same fault-don't-guess discipline the ISS itself is held to.
"""
import re

SETUP_ASM = "movxm\t{reg}, #{val}"
SPILL_ASM = "st\t{reg}, [{ptr}, #{off}]"

# AIE2PRegisterInfo.td: mMvSclDst = (add eR, mDm, eP, eS, le, ls, lr, sp, mCRm,
# mSRm, mCRFP, lc) -- what MOVXM's OP_mMvSclDstCg destination can reach.
_MOVXM_CLASSES = {"eR", "mDm", "eP", "eS", "le", "ls", "lr", "sp", "mCRm", "mSRm", "mCRFP", "lc"}

# AIE2PRegisterInfo.td: mSclSt = (add eP, eR, mDm, mEs, lr) -- what
# ST_dms_sts_idx_imm's OP_mSclSt source can reach.
_SPILL_CLASSES = {"eR", "eP", "mDm", "mEs", "lr"}


def render(asm_template, values):
    """Substitute $name tokens in a TableGen AsmString. Longest names first so a
    short name cannot partially match inside a longer one.
    """
    out = asm_template
    for name in sorted(values, key=len, reverse=True):
        out = re.sub(r"\$" + re.escape(name) + r"\b", values[name], out)
    return out


def gen_kernel(manifest, reg_of, seed_of, imm_of, ptr_reg="p0", spill_base=0, spill_stride=4):
    """manifest: a Stage-1 dict from extract_opcode.manifest().
    reg_of:  {operand_name: concrete register string}, for every register-class
             operand, input or output.
    seed_of: {operand_name: literal string}, the value MOVXM loads into a
             register-class INPUT operand before the instruction runs.
    imm_of:  {operand_name: literal string}, for operands that are themselves an
             assembler immediate (substituted directly, no register, no MOVXM).
    Returns (lines, expectations); expectations maps each spilled output operand
    name to (offset, register) -- what a real run's DMA-back would need to check.
    """
    lines = []

    for cls, name in manifest["ins"]:
        if name in imm_of:
            continue
        if cls not in _MOVXM_CLASSES:
            raise NotImplementedError(
                f"{manifest['opcode']}: input operand ${name} is class {cls}, "
                "outside the MOVXM-reachable set this generator knows how to seed"
            )
        lines.append(SETUP_ASM.format(reg=reg_of[name], val=seed_of[name]))

    values = dict(reg_of)
    values.update(imm_of)
    lines.append(render(manifest["asm"], values))

    expectations = {}
    for i, (cls, name) in enumerate(manifest["outs"]):
        if cls not in _SPILL_CLASSES:
            raise NotImplementedError(
                f"{manifest['opcode']}: output operand ${name} is class {cls}, "
                "outside the ST_dms_sts_idx_imm-reachable set this generator knows how to spill"
            )
        off = spill_base + i * spill_stride
        lines.append(SPILL_ASM.format(reg=reg_of[name], ptr=ptr_reg, off=off))
        expectations[name] = (off, reg_of[name])

    return lines, expectations
