results/exp_a_sim_load0.60.csv: 50 seeds, workers=9, jobs=2000, load=0.600

| metric | round-robin | first-fit | best-fit |
|---|---|---|---|
| makespan (s) | 462.8 (sd 8.3) | 462.7 (sd 8.3) | 462.7 (sd 8.2) |
| CPU util while queue non-empty | 0.8027 (sd 0.027) | 0.8228 (sd 0.21) | 0.7773 (sd 0.32) |
| mem util while queue non-empty | 0.4545 (sd 0.042) | 0.4807 (sd 0.14) | 0.4438 (sd 0.19) |
| CPU util over makespan | 0.581 (sd 0.023) | 0.5812 (sd 0.023) | 0.5812 (sd 0.023) |
| fragmentation (frac of busy time) | 0.9919 (sd 0.02) | 0.9013 (sd 0.24) | 0.7756 (sd 0.35) |
| queue wait p50 (s) | 0 (sd 0) | 0 (sd 0) | 0 (sd 0) |
| queue wait p99 (s) | 1.122 (sd 0.62) | 0.3692 (sd 0.49) | 0.2456 (sd 0.42) |

Paired difference vs **first-fit** (policy minus first-fit, mean +/- 95% CI, wins/losses/ties by seed)
A 'win' is the better direction for that metric. Differences whose CI contains 0 are not distinguishable from noise at this n.

| metric | round-robin | best-fit |
|---|---|---|
| makespan (s) | +0.1404 +/- 0.13 (0/6/44) | +0.0174 +/- 0.042 (1/1/48) |
| CPU util while queue non-empty | -0.02017 +/- 0.057 (4/46/0) | -0.04548 +/- 0.069 (36/10/4) |
| mem util while queue non-empty | -0.02616 +/- 0.034 (10/40/0) | -0.03691 +/- 0.037 (24/22/4) |
| CPU util over makespan | -0.000174 +/- 0.00016 (0/6/44) | -2.4e-05 +/- 5.7e-05 (1/1/48) |
| fragmentation (frac of busy time) | +0.09067 +/- 0.069 (2/20/28) | -0.1257 +/- 0.079 (26/2/22) |
| queue wait p50 (s) | +0 +/- 0 (0/0/50) | +0 +/- 0 (0/0/50) |
| queue wait p99 (s) | +0.7526 +/- 0.1 (0/49/1) | -0.1236 +/- 0.057 (25/3/22) |
