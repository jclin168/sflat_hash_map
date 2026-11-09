#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check that the code between the sflat-branchless markers has no
conditional branch.

The markers are SFLAT_BRANCHLESS_BEGIN() and SFLAT_BRANCHLESS_END() (see
include/sflat/flat_hash_map.hpp); they are empty asm statements when
SFLAT_CHECK_BRANCHLESS is defined. This script compiles
tools/branchless_check.cpp to assembly with each compiler and flag set, and
follows each marked region from its begin marker to the end marker. An
unconditional jump is followed (compilers share one tail between regions);
a conditional branch, a return or a call fails the check.

Usage, from the repository root:
  tools/check_branchless.py [compiler ...]     (default: g++ clang++)
For AArch64 (NEON) on an x86-64 host, give a cross compiler, for example
aarch64-linux-gnu-g++, or a script that runs
clang++ --target=aarch64-linux-gnu.
Exit status: 0 all checks pass, 1 a check fails, 2 no compiler found.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

# Flag sets that a compiler does not take (x86 flags for an AArch64
# compiler, and the other way round) are skipped.
FLAGS = ["-O2", "-O3", "-O3 -march=native", "-O3 -march=x86-64-v3",
         "-O3 -march=armv8.2-a", "-O3 -mcpu=apple-m1",
         "-O3 -DSFLAT_NO_SIMD=1"]
BEGIN, END = "sflat-branchless-begin", "sflat-branchless-end"
# x86-64 and AArch64 mnemonics.
UNCOND = {"jmp", "b"}
COND = re.compile(r"^(j(?!mp)[a-z]+|b\.[a-z]+|cbn?z|tbn?z)$")
STOP = re.compile(r"^(ret[a-z]*|call[a-z]*|bl|blr|br)$")
LABEL = re.compile(r"^([.\w$]+):")


def check(lines):
    """Return (regions, errors) for the assembly lines."""
    labels = {}
    for i, l in enumerate(lines):
        m = LABEL.match(l)
        if m:
            labels[m.group(1)] = i
    errors, regions = [], 0
    for start, l in enumerate(lines):
        if BEGIN not in l:
            continue
        regions += 1
        i, steps, seen = start + 1, 0, set()
        while True:
            if i >= len(lines) or steps > 10000:
                errors.append(f"line {start + 1}: region has no end")
                break
            if i in seen:
                errors.append(f"line {start + 1}: region loops")
                break
            seen.add(i)
            steps += 1
            l = lines[i].strip()
            if END in l:
                break
            if BEGIN in l:
                errors.append(f"line {start + 1}: begin before end")
                break
            f = l.split()
            op = f[0] if f else ""
            if op in UNCOND and len(f) > 1 and f[1] in labels:
                i = labels[f[1]] + 1
                continue
            if COND.match(op) or op in UNCOND:
                errors.append(f"line {i + 1}: {l}")
                break
            if STOP.match(op):
                errors.append(f"line {i + 1}: {l} before the end marker")
                break
            i += 1
    return regions, errors


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    src = os.path.join(root, "tools", "branchless_check.cpp")
    inc = os.path.join(root, "include")
    compilers = sys.argv[1:] or ["g++", "clang++"]
    status, checked = 0, 0
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "out.s")
        for cxx in compilers:
            if not shutil.which(cxx):
                print(f"skip {cxx} (not found)")
                continue
            for flags in FLAGS:
                cmd = [cxx, "-std=c++17", *flags.split(), "-DNDEBUG",
                       "-DSFLAT_CHECK_BRANCHLESS", "-I", inc, "-S", "-o", out,
                       src]
                r = subprocess.run(cmd, capture_output=True, text=True)
                if r.returncode != 0:
                    print(f"skip {cxx} {flags} (does not compile here)")
                    continue
                checked += 1
                with open(out) as f:
                    regions, errors = check(f.read().splitlines())
                if regions == 0:
                    errors = ["no marked region in the output"]
                if errors:
                    status = 1
                    print(f"FAIL {cxx} {flags}:")
                    for e in errors:
                        print(f"    {e}")
                else:
                    print(f"ok   {cxx} {flags} ({regions} regions)")
    if checked == 0:
        print("no compiler found")
        return 2
    return status


if __name__ == "__main__":
    sys.exit(main())
