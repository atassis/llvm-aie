#!/usr/bin/env python3
"""Device-free tests for the Stage-1/Stage-2 differential-harness slice.

ExtractOpcodeTest needs a built llvm-tblgen and skips (does not fail, does not
fake a pass) when AIE2P_ISA_ORACLE_TBLGEN is unset -- see the class docstring for
why this tree's own build-asserts cannot supply one today.

GenTestKernelTest needs neither tblgen nor a build: its manifests are inlined so
the generator is checked against fixed, known TableGen output rather than against
whatever a live extraction happens to return.
"""
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import extract_opcode as eo  # noqa: E402
import gen_test_kernel as gk  # noqa: E402

TBLGEN = os.environ.get("AIE2P_ISA_ORACLE_TBLGEN")
AIE_ROOT = Path(__file__).resolve().parents[2]  # .../llvm
CACHE = Path("/tmp/aie2p_isa_oracle_test_records.json")


class ExtractOpcodeTest(unittest.TestCase):
    """Runs llvm-tblgen --dump-json over THIS tree's own AIE2P.td and checks the
    result against known-good values, so a change to the .td that silently moves
    an operand or a mnemonic is caught here rather than downstream in a generated
    kernel that looks fine and asserts the wrong thing.
    """

    @classmethod
    def setUpClass(cls):
        if not TBLGEN or not Path(TBLGEN).is_file():
            raise unittest.SkipTest(
                "AIE2P_ISA_ORACLE_TBLGEN not set to a built llvm-tblgen. This "
                "repo's own llvm-aie/build-asserts is not a usable source: its "
                "CMakeCache.txt points CMAKE_HOME_DIRECTORY at an unrelated "
                "checkout (forks/llvm-aie-bisect), so re-running ninja there "
                "fails before touching any AIE2P target. Point this at a "
                "NATIVE/bin/llvm-tblgen from any llvm-aie build tree instead; "
                "the binary itself has no path baked in and works standalone."
            )
        eo.dump_json(Path(TBLGEN), AIE_ROOT, CACHE)
        cls.records = eo.load_records(CACHE)

    def test_control_family_present(self):
        # Named in docs/kb/aie2p-oracle-is-the-gap.md as the harness's CONTROL:
        # "scalar ALU / LDA / PADDS, which should agree -- without it a failure
        # is unattributable".
        for name in ("ADD_add_r_ri", "LDA_s16_idx_imm", "PADDS_pstm_nrm_imm"):
            self.assertIn(name, self.records)

    def test_probe_family_vsrs_vups_count(self):
        # Same KB entry: "Probe: VSRS/VUPS, 16 opcodes already implemented".
        vsrs = eo.family(self.records, "VSRS_")
        vups = eo.family(self.records, "VUPS_")
        self.assertEqual(len(vsrs) + len(vups), 16, (vsrs, vups))

    def test_add_manifest_shape(self):
        m = eo.manifest(self.records, "ADD_add_r_ri")
        self.assertEqual(m["asm"], "add\t$d0, $s0, #$imm")
        self.assertEqual(m["ins"], [("eR", "s0"), ("c7s", "imm")])
        self.assertEqual(m["outs"], [("eR", "d0")])
        self.assertEqual(m["defs"], ["srCarry"])


class GenTestKernelTest(unittest.TestCase):
    _ADD_MANIFEST = {
        "opcode": "ADD_add_r_ri",
        "asm": "add\t$d0, $s0, #$imm",
        "ins": [("eR", "s0"), ("c7s", "imm")],
        "outs": [("eR", "d0")],
        "defs": ["srCarry"],
        "uses": [],
    }

    def test_add_r_ri_kernel(self):
        lines, expect = gk.gen_kernel(
            self._ADD_MANIFEST,
            reg_of={"s0": "r0", "d0": "r1"},
            seed_of={"s0": "7"},
            imm_of={"imm": "5"},
        )
        self.assertEqual(lines, [
            "movxm\tr0, #7",
            "add\tr1, r0, #5",
            "st\tr1, [p0, #0]",
        ])
        self.assertEqual(expect, {"d0": (0, "r1")})

    def test_unsupported_output_class_faults_not_guesses(self):
        manifest = {
            "opcode": "FAKE",
            "asm": "fake\t$d0, $s0",
            "ins": [("eR", "s0")],
            "outs": [("eQYs", "d0")],  # a sparse class, deliberately outside the spill set
            "defs": [],
            "uses": [],
        }
        with self.assertRaises(NotImplementedError):
            gk.gen_kernel(manifest, reg_of={"s0": "r0", "d0": "q0"}, seed_of={"s0": "1"}, imm_of={})

    def test_unsupported_input_class_faults_not_guesses(self):
        manifest = {
            "opcode": "FAKE2",
            "asm": "fake2\t$d0, $s0",
            "ins": [("eQYs", "s0")],  # a sparse class, deliberately outside the seed set
            "outs": [("eR", "d0")],
            "defs": [],
            "uses": [],
        }
        with self.assertRaises(NotImplementedError):
            gk.gen_kernel(manifest, reg_of={"s0": "q0", "d0": "r1"}, seed_of={"s0": "1"}, imm_of={})


if __name__ == "__main__":
    unittest.main()
