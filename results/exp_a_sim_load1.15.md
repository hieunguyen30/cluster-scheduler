results/exp_a_sim_load1.15.csv: 50 seeds, workers=9, jobs=2000, load=1.150

| metric | round-robin | first-fit | best-fit |
|---|---|---|---|
| makespan (s) | 314.9 (sd 10) | 310 (sd 10) | 307.3 (sd 10) |
| CPU util while queue non-empty | 0.8911 (sd 0.0081) | 0.9061 (sd 0.0072) | 0.9146 (sd 0.0058) |
| mem util while queue non-empty | 0.5889 (sd 0.018) | 0.5988 (sd 0.018) | 0.6044 (sd 0.018) |
| CPU util over makespan | 0.8537 (sd 0.011) | 0.8673 (sd 0.011) | 0.8749 (sd 0.01) |
| fragmentation (frac of busy time) | 0.9281 (sd 0.023) | 0.8476 (sd 0.03) | 0.8161 (sd 0.032) |
| queue wait p50 (s) | 33.54 (sd 6.7) | 30.88 (sd 6.9) | 29.54 (sd 6.6) |
| queue wait p99 (s) | 64.43 (sd 10) | 59.64 (sd 10) | 56.95 (sd 10) |

Paired difference vs **first-fit** (policy minus first-fit, mean +/- 95% CI, wins/losses/ties by seed)
A 'win' is the better direction for that metric. Differences whose CI contains 0 are not distinguishable from noise at this n.

| metric | round-robin | best-fit |
|---|---|---|
| makespan (s) | +4.902 +/- 0.8 (0/50/0) | -2.725 +/- 0.58 (45/5/0) |
| CPU util while queue non-empty | -0.01494 +/- 0.0026 (1/49/0) | +0.008566 +/- 0.0018 (47/3/0) |
| mem util while queue non-empty | -0.00994 +/- 0.0017 (1/49/0) | +0.005512 +/- 0.0012 (47/3/0) |
| CPU util over makespan | -0.01353 +/- 0.0022 (0/50/0) | +0.007676 +/- 0.0017 (45/5/0) |
| fragmentation (frac of busy time) | +0.0805 +/- 0.009 (0/50/0) | -0.0315 +/- 0.011 (39/11/0) |
| queue wait p50 (s) | +2.655 +/- 0.59 (5/44/1) | -1.341 +/- 0.38 (45/5/0) |
| queue wait p99 (s) | +4.782 +/- 0.81 (1/49/0) | -2.692 +/- 0.57 (46/4/0) |
