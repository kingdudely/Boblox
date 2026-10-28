#!/usr/bin/env python3
"""zstd_lib.py — libzstd via ctypes, one loader for the tool scripts.

challenge_blob.py and repl_survey.py each wrapped ZSTD_decompress by hand
(different sizing rules, different error text). This keeps the safe rule — the
frame's declared content size when it has one, a 64 MiB cap otherwise — in a
single place.
"""
import ctypes
import ctypes.util

_lib = None


def _zstd():
    global _lib
    if _lib is None:
        path = ctypes.util.find_library("zstd") or "libzstd.so.1"
        lib = ctypes.CDLL(path)
        lib.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                        ctypes.c_void_p, ctypes.c_size_t]
        lib.ZSTD_decompress.restype = ctypes.c_size_t
        lib.ZSTD_isError.argtypes = [ctypes.c_size_t]
        lib.ZSTD_isError.restype = ctypes.c_uint
        lib.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
        lib.ZSTD_getErrorName.restype = ctypes.c_char_p
        lib.ZSTD_getFrameContentSize.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        lib.ZSTD_getFrameContentSize.restype = ctypes.c_ulonglong
        _lib = lib
    return _lib


def decompress(data, max_out=0):
    """One zstd frame -> bytes. max_out <= 0 asks the frame (64 MiB cap)."""
    lib = _zstd()
    if max_out <= 0:
        max_out = lib.ZSTD_getFrameContentSize(data, len(data))
        if max_out in (0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFE) or max_out == 0:
            max_out = 64 * 1024 * 1024
    src = ctypes.create_string_buffer(data, len(data))
    dst = ctypes.create_string_buffer(max_out)
    r = lib.ZSTD_decompress(dst, max_out, src, len(data))
    if lib.ZSTD_isError(r):
        raise ValueError("zstd: %s" % lib.ZSTD_getErrorName(r).decode())
    return bytes(dst.raw[:r])
