#!/usr/bin/env python3
"""Stage 1 of an AIE2P differential-harness slice: read an opcode's operand shape
and exact assembly syntax straight from the backend's own TableGen records,
instead of inferring either from a mnemonic.

Requires an llvm-tblgen built close enough to the llvm-aie tree being queried.
--dump-json is a stable interchange format across nearby revisions of one target,
not guaranteed stable across the fork's whole history.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path


def dump_json(tblgen, aie_root, out_path):
    """Run llvm-tblgen --dump-json over AIE2P.td and write out_path.

    aie_root is a llvm-aie checkout's llvm/ directory.
    """
    aie_dir = aie_root / "lib" / "Target" / "AIE"
    aie2p_td = aie_dir / "aie2p" / "AIE2P.td"
    if not aie2p_td.exists():
        raise FileNotFoundError(aie2p_td)
    cmd = [
        str(tblgen),
        "--dump-json",
        "-I", str(aie_dir),
        "-I", str(aie_root / "include"),
        "-I", str(aie_root / "lib" / "Target"),
        "-I", str(aie_dir / "aie2p"),
        str(aie2p_td),
        "-o", str(out_path),
    ]
    subprocess.run(cmd, check=True)
    return out_path


def load_records(json_path):
    with open(json_path) as f:
        return json.load(f)


def instruction_names(records):
    return records["!instanceof"].get("Instruction", [])


def family(records, prefix):
    """Every instruction whose name starts with prefix, sorted. Used to turn the
    KB's "16 opcodes" style claim into a checkable set instead of a quoted count.
    """
    return sorted(n for n in instruction_names(records) if n.startswith(prefix))


def _operand_list(dag):
    """A TableGen (ins ...)/(outs ...) dag -> [(register_or_imm_class, arg_name), ...]."""
    return [(ref["def"], name) for ref, name in dag.get("args", [])]


def manifest(records, opcode):
    """The Stage-1 deliverable for one opcode: its operand classes, in order, and
    its literal AsmString, both read from the record TableGen already builds for
    the real backend -- nothing here is guessed from the opcode's name.
    """
    r = records[opcode]
    return {
        "opcode": opcode,
        "asm": r.get("AsmString", ""),
        "ins": _operand_list(r["InOperandList"]),
        "outs": _operand_list(r["OutOperandList"]),
        "defs": [d["def"] for d in r.get("Defs", [])],
        "uses": [d["def"] for d in r.get("Uses", [])],
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tblgen", required=True, type=Path)
    ap.add_argument("--aie-root", required=True, type=Path,
                     help="llvm-aie checkout's llvm/ directory")
    ap.add_argument("--cache", type=Path, default=None,
                     help="reuse an existing --dump-json output instead of re-running tblgen")
    ap.add_argument("opcode_or_prefix")
    ap.add_argument("--family", action="store_true",
                     help="treat the argument as a name PREFIX and list every match")
    args = ap.parse_args(argv)

    json_path = args.cache or Path("/tmp/aie2p_isa_oracle_records.json")
    if args.cache is None:
        dump_json(args.tblgen, args.aie_root, json_path)
    records = load_records(json_path)

    if args.family:
        for n in family(records, args.opcode_or_prefix):
            print(n)
    else:
        print(json.dumps(manifest(records, args.opcode_or_prefix), indent=2))


if __name__ == "__main__":
    sys.exit(main())
