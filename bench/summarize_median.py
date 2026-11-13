#!/usr/bin/env python3
"""Median of several bench runs -> markdown tables. Usage: summarize_median.py run1.log run2.log ..."""
import re, sys, statistics as st
from collections import defaultdict
FIELDS = [("insert","insert (reserved)"),("find_hit","find hit (insert order)"),
          ("find_hit_rand","find hit (random order)"),("find_miss","find miss"),
          ("erase","erase"),("iter","iterate")]
d = defaultdict(list); order = []; n = None
for f in sys.argv[1:]:
    for line in open(f):
        m = re.match(r"=== n = (\d+)", line)
        if m: n = int(m.group(1)); continue
        m = re.match(r"([AB]) (\S+)\s+n=(\d+)\s+(.*)", line)
        if m:
            k, name, nn, rest = m.groups()
            if name not in order: order.append(name)
            for key, val in re.findall(r"(\w+)=\s*([\d.]+)", rest.split("ns/op")[0]):
                d[(int(nn), name, key)].append(float(val))
            continue
        m = re.match(r"\s+bytes/entry \((steady|growth peak)\):(.*)", line)
        if m:
            for name, val in re.findall(r"(\S+)=([\d.-]+)", m.group(2)):
                d[(n, name, "mem_" + m.group(1))].append(float(val))
med = lambda n, nm, k: st.median(d[(n, nm, k)]) if d[(n, nm, k)] else None
sizes = sorted({k[0] for k in d})
for n in sizes:
    print(f"**n = {n:,}**\n")
    print("| Operation | " + " | ".join(order) + " |")
    print("|---|" + "---|" * len(order))
    rows = [(k, l) for k, l in FIELDS] + [("insert_noreserve", "insert (no reserve)"),
            ("mem_steady", "bytes/entry (steady)"), ("mem_growth peak", "bytes/entry (growth peak)")]
    for k, label in rows:
        v = [med(n, nm, k) for nm in order]
        best = min(x for x in v if x is not None)
        cells = [("**%.1f**" if x == best else "%.1f") % x if x is not None else "-" for x in v]
        print(f"| {label} | " + " | ".join(cells) + " |")
    print()
    # spread
    print("Run spread (min-max, ns/op) of sflat and ankerl:\n")
    for nm in ("sflat", "ankerl"):
        print(f"- {nm}: " + ", ".join(f"{l} {min(d[(n,nm,k)]):.1f}-{max(d[(n,nm,k)]):.1f}"
              for k, l in FIELDS[:1] + FIELDS[2:4] + [("insert_noreserve","no reserve")] if d[(n,nm,k)]))
    print()
