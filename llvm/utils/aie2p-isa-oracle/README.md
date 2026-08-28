# AIE2P ISA oracle harness -- Stage 1 and 2

Two device-free stages of a differential-harness slice for AIE2P instruction
semantics (design: see the fork's own knowledge base, "the oracle is the gap").
No hardware oracle exists for what these opcodes do on silicon; this is the
scaffolding that will eventually feed one, not the oracle itself.

- `extract_opcode.py` -- Stage 1. Runs `llvm-tblgen --dump-json` over
  `AIE2P.td` and reads an opcode's operand classes and exact `AsmString`
  straight from the backend's own tables, so nothing downstream has to guess a
  register class or a mnemonic from a name.
- `gen_test_kernel.py` -- Stage 2. Given a Stage-1 manifest for a CONTROL-family
  opcode, emits the assembly to seed its register inputs (`movxm`), run it, and
  spill its register outputs (`st ... idx_imm`). Raises on any operand class
  outside the two families it knows how to drive, rather than emitting a
  plausible-looking kernel that reads the wrong bits.
- `test_isa_oracle.py` -- exercises both against real TableGen output. The
  extraction tests need a built `llvm-tblgen` (`AIE2P_ISA_ORACLE_TBLGEN=/path/to/llvm-tblgen`)
  and skip, rather than fail, without one. The generator tests run unconditionally
  against fixed manifests.

## What this is not

Neither stage runs an instruction anywhere. Turning a generated kernel into a
real answer needs: assembling it (`llvm-mc`), packaging it for a device
(`aiecc.py` -> xclbin), running it on real Strix silicon and DMA'ing the result
back, and running the same kernel through the AIE2P ISS for comparison. All of
that is out of scope here -- most of it needs a device, and the ISS itself is
not yet upstream.
