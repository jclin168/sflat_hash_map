# Miss early exit (SFLAT_MISS_EMPTY_EXIT), Neoverse-V2, 2026-11-11

Measured first on local base 373ddf8 (older main + erase fix), then again on main aae65ea
(probe_gated_loads_on_aae65ea.txt, below).

Change: in find_ptr(), when the table is not migrating and load (size+deleted)/cap < 5/8,
return a miss if the home group has a kEmpty slot, before the lazy overflow-word read.
Invariant: a key spills past its home group only when the group is full, and free_ctrl never
gives an empty slot back to a group a probe passed through.

Cause found with jahb (2M uint32): sflat load at 2M = 0.477 (same as absl/ankerl), prefetch off
did not change miss -> miss cost was ctrl read then serial overflow read.

probe.cpp uint64->uint64, reserved, min of 3 reps, ns (base = 373ddf8):
- Ungated exit: load 0.87 miss slower (1M ~10 -> ~14, 10M ~23 -> ~29, 100M ~43 -> ~47).
- Ungated by load (10M miss): 0.50 20.5 -> 17.7; 0.60 ~20.5 -> ~19.5; 0.70 tie/worse; 0.80 22.2 -> 26.1.
- Gated at 5/8 (final), 3 rounds, 10M miss: 0.50 20.2/20.3/20.7 -> 17.2/17.1/17.9;
  0.60 19.9/19.5/21.1 -> 19.4/19.0/19.9; 0.87 22.3/24.1/23.9 -> 22.5/23.9/24.6 (tie).
  1M miss 0.50 9.7/11.4/11.7 -> 8.6/9.5/9.6. Hit unchanged. Insert within noise.
jahb 2M (ungated, load 0.477): look up nonexisting -12..15%, erase nonexisting ~-14%.
jahb artifact: look-up-nonexisting runs right after look-up-existing; without the hit loop
sflat miss 33.5/39.0 vs absl 28.6/31.3 (with it: 49 vs 33).

On main aae65ea (up) vs this change (new), same probe, 3 rounds, miss ns:
- 10M: 0.50 load 19.5/19.4/21.3 -> 18.1/16.7/17.7; 0.60 19.8/19.4/19.8 -> 18.5/18.5/18.5;
  0.87 22.3/22.0/22.4 -> 22.1/23.3/22.7 (no change).
- 1M: 0.50 12.0/11.0/10.5 -> 10.4/8.5/9.5; 0.60 and 0.87 no change.
- Hit and reserved insert: no change outside noise.
