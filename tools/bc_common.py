#!/usr/bin/env python3
"""bc_common.py — the bytecode varint reader shared by the decoder tools.

Six decoders (decode_roblox_bc, decode_wire_bc, opcode_map, standardize_wire,
disasm_roblox_bc, patch_decoded_bc) each carried the same ten-line LEB128
reader. It lives here once so a fix (bounds check, 64-bit guard) lands in all
of them.

Not to be confused with the *compact* varint used by wire framing and
replication (top-2-bit size scheme): that is a different encoding and stays in
py/msgbuild.py, tools/native_ch.py and tools/repl_survey.py.
"""


def varint(buf, off):
    """LEB128: returns (value, offset_after).

    Callers index past the end at their own risk, exactly like the copies this
    replaces — the decoders validate lengths before calling.
    """
    v = 0
    sh = 0
    while True:
        b = buf[off]
        off += 1
        v |= (b & 0x7F) << sh
        if not (b & 0x80):
            return v, off
        sh += 7
