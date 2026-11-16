#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise malformed cross-page TU records against the format checker."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import argparse
import struct
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
CHECKER_PATH = HERE / "check-cross-page-tu-format.py"
SPEC = importlib.util.spec_from_file_location(
    "cross_page_tu_format", CHECKER_PATH)
assert SPEC and SPEC.loader
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)

PAGE_SIZE = 4096
PAGE_PREFIX_SIZE = 16 + PAGE_SIZE
TAIL_SIZE = CHECKER.AOT_TAIL_SIZE
AOT_TU_FLAG_BIT = 10
PUSHPOP_BIT = 47
TU_ID = 0x123456789ABCDEF0
ANCHOR_PC = 0x1000
TU_HEAD_SIZE = 40
TU_PAGE_SIZE = 16
AOT_BLOCK_SIZE = 176
ZERO_PAGE_FINGERPRINT = CHECKER.page_fingerprint(bytes(PAGE_SIZE))


def make_block(pc: int) -> bytearray:
    block = bytearray(AOT_BLOCK_SIZE)
    struct.pack_into("<QQQ", block, 0, pc, 0, (1 << 64) - 1)
    struct.pack_into("<ii", block, 128, -1, -2)
    struct.pack_into("<HH", block, 152, 0xFFFF, 0xFFFF)
    return block


def make_cache(*, flags: int = 1 << AOT_TU_FLAG_BIT,
               search_size: int = 0, icount: int = 0,
               extra_tu_bytes: int = 0) -> tuple[bytearray, dict[str, int]]:
    blocks = make_block(ANCHOR_PC) + make_block(ANCHOR_PC + PAGE_SIZE)
    struct.pack_into("<Q", blocks, 56, search_size)
    struct.pack_into("<H", blocks, 158, icount)
    dependencies = struct.pack(
        "<qQqQ", 0, ZERO_PAGE_FINGERPRINT,
        PAGE_SIZE, ZERO_PAGE_FINGERPRINT
    )
    tu_size = TU_HEAD_SIZE + len(dependencies) + len(blocks) + extra_tu_bytes
    tu = bytearray(struct.pack(
        "<IIIIQQII", CHECKER.TB_MAGIC, tu_size, 2, 0,
        ANCHOR_PC, TU_ID, 2, 0
    ))
    tu += dependencies
    blocks_offset = len(tu)
    tu += blocks
    tu += bytes(extra_tu_bytes)

    anchor_size = PAGE_PREFIX_SIZE + len(tu)
    anchor = bytearray(struct.pack(
        "<IIIi", CHECKER.PAGE_MAGIC, 0x11111111, 1, anchor_size
    ))
    anchor += bytes(PAGE_SIZE)
    tu_offset = len(anchor)
    anchor += tu

    ref = struct.pack(
        "<IIQqII", CHECKER.TU_REF_MAGIC, CHECKER.TU_REF.size,
        TU_ID, -PAGE_SIZE, 0, 0
    )
    member_size = PAGE_PREFIX_SIZE + len(ref)
    member = bytearray(struct.pack(
        "<IIIi", CHECKER.PAGE_MAGIC, 0x22222222, 1, member_size
    ))
    member += bytes(PAGE_SIZE)
    ref_offset = len(anchor) + len(member)
    member += ref

    page_data = anchor + member
    page_info = struct.pack(
        "<I4xI4xQI4xQ", 2, 0x11111111, 0,
        0x22222222, len(anchor)
    )
    tail = bytearray(TAIL_SIZE)
    struct.pack_into("<Qii", tail, 0, flags, 2, 2)
    cache = page_data + page_info + tail
    return cache, {
        "tu": tu_offset,
        "blocks": tu_offset + blocks_offset,
        "ref": ref_offset,
        "member_guest": ref_offset - PAGE_SIZE,
    }


def make_duplicate_single_page_cache() -> bytearray:
    block = make_block(ANCHOR_PC)
    dependency = struct.pack("<qQ", 0, ZERO_PAGE_FINGERPRINT)
    tu_size = TU_HEAD_SIZE + len(dependency) + len(block)
    tu = struct.pack(
        "<IIIIQQII", CHECKER.TB_MAGIC, tu_size, 1, 0,
        ANCHOR_PC, TU_ID, 1, 0
    ) + dependency + block
    page_size = PAGE_PREFIX_SIZE + len(tu)
    page = (struct.pack("<IIIi", CHECKER.PAGE_MAGIC, 0x11111111,
                        1, page_size) + bytes(PAGE_SIZE) + tu)
    page_info = struct.pack(
        "<I4xI4xQI4xQ", 2, 0x11111111, 0,
        0x11111111, len(page)
    )
    tail = bytearray(TAIL_SIZE)
    struct.pack_into("<Qii", tail, 0, 1 << AOT_TU_FLAG_BIT, 2, 2)
    return bytearray(page + page + page_info + tail)


def make_duplicate_cross_page_cache() -> bytearray:
    valid, offsets = make_cache()
    anchor_size = struct.unpack_from("<i", valid, 12)[0]
    member_offset = offsets["ref"] - PAGE_PREFIX_SIZE
    member_size = struct.unpack_from("<i", valid, member_offset + 12)[0]
    pair_size = member_offset + member_size
    page_data = valid[:pair_size] + valid[:pair_size]
    page_info = struct.pack(
        "<I4xI4xQI4xQI4xQI4xQ", 4,
        0x11111111, 0,
        0x22222222, anchor_size,
        0x11111111, pair_size,
        0x22222222, pair_size + anchor_size
    )
    tail = bytearray(TAIL_SIZE)
    struct.pack_into("<Qii", tail, 0, 1 << AOT_TU_FLAG_BIT, 4, 4)
    return bytearray(page_data + page_info + tail)


def run_runtime_checker(
    checker: Path, cache: Path,
) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(checker), str(cache)], capture_output=True, text=True, check=False
    )


def expect_rejected(name: str, cache: bytearray, message: str,
                    runtime_checker: Path | None) -> None:
    with tempfile.NamedTemporaryFile() as stream:
        stream.write(cache)
        stream.flush()
        try:
            CHECKER.parse_cache(Path(stream.name))
        except ValueError as exc:
            if message not in str(exc):
                raise AssertionError(
                    f"{name}: expected {message!r}, got {str(exc)!r}"
                ) from exc
        else:
            raise AssertionError(f"{name}: malformed cache was accepted")
        if runtime_checker:
            result = run_runtime_checker(runtime_checker, Path(stream.name))
            if result.returncode != 1:
                raise AssertionError(
                    f"{name}: runtime checker returned {result.returncode}: "
                    f"{result.stderr.strip()}"
                )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime-checker", type=Path)
    args = parser.parse_args()

    valid, valid_offsets = make_cache()
    if CHECKER.parse_cache_bytes(valid) != (2, 1, 2, 1, 2):
        raise AssertionError("valid cross-page TU fixture was rejected")
    if args.runtime_checker:
        with tempfile.NamedTemporaryFile() as stream:
            stream.write(valid)
            stream.flush()
            result = run_runtime_checker(args.runtime_checker,
                                         Path(stream.name))
            if result.returncode != 0:
                raise AssertionError(
                    f"valid fixture: runtime checker returned "
                    f"{result.returncode}: {result.stderr.strip()}"
                )

    duplicate_single_page = make_duplicate_single_page_cache()
    if CHECKER.parse_cache_bytes(duplicate_single_page) != (2, 2, 2, 0, 1):
        raise AssertionError("duplicate single-page TU fixture was rejected")
    if args.runtime_checker:
        with tempfile.NamedTemporaryFile() as stream:
            stream.write(duplicate_single_page)
            stream.flush()
            result = run_runtime_checker(args.runtime_checker,
                                         Path(stream.name))
            if result.returncode != 0:
                raise AssertionError(
                    f"duplicate single-page fixture: runtime checker returned "
                    f"{result.returncode}: {result.stderr.strip()}"
                )

    expect_rejected("duplicate cross-page payload",
                    make_duplicate_cross_page_cache(),
                    "cross-page TU payload", args.runtime_checker)

    cache = bytearray(valid)
    struct.pack_into("<Q", cache, valid_offsets["blocks"] + AOT_BLOCK_SIZE,
                     ANCHOR_PC + 2 * PAGE_SIZE)
    expect_rejected("missing dependency", cache, "dependency missing",
                    args.runtime_checker)

    cache = bytearray(valid)
    struct.pack_into("<Q", cache, valid_offsets["tu"] + 16, ANCHOR_PC + 1)
    expect_rejected("first_pc mismatch", cache, "entry mismatch",
                    args.runtime_checker)

    cache, offsets = make_cache(search_size=4)
    expect_rejected("truncated search payload", cache, "payload size mismatch",
                    args.runtime_checker)

    cache, offsets = make_cache(flags=(1 << AOT_TU_FLAG_BIT) |
                                (1 << PUSHPOP_BIT), icount=1)
    expect_rejected("truncated push/pop payload", cache,
                    "payload size mismatch", args.runtime_checker)

    cache, offsets = make_cache(extra_tu_bytes=4)
    expect_rejected("false tu_sum_size", cache, "payload size mismatch",
                    args.runtime_checker)

    cache = bytearray(valid)
    struct.pack_into("<I", cache, valid_offsets["tu"] + 32, 9)
    expect_rejected("too many TU pages", cache, "invalid TU header",
                    args.runtime_checker)

    cache = bytearray(valid)
    struct.pack_into("<Q", cache, valid_offsets["ref"] + 8, TU_ID + 1)
    expect_rejected("bad member reference", cache,
                    "member references mismatch", args.runtime_checker)

    cache = bytearray(valid)
    cache[valid_offsets["member_guest"]] ^= 1
    expect_rejected("reference on wrong guest page", cache,
                    "member references mismatch", None)

    cache = bytearray(valid)
    struct.pack_into("<q", cache, valid_offsets["tu"] + TU_HEAD_SIZE,
                     -(1 << 63))
    expect_rejected("unrepresentable dependency", cache,
                    "invalid TU dependency", args.runtime_checker)

    print("PASS: malformed cross-page TU fixtures are rejected")


if __name__ == "__main__":
    main()
