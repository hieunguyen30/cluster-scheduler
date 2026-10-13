results/exp_a_sim_load1.00.csv: 50 seeds, workers=9, jobs=2000, load=1.000

| metric | round-robin | first-fit | best-fit |
|---|---|---|---|
| makespan (s) | 316 (sd 9.8) | 311.4 (sd 10) | 308.8 (sd 9.4) |
| CPU util while queue non-empty | 0.8907 (sd 0.0068) | 0.906 (sd 0.0082) | 0.9144 (sd 0.0067) |
| mem util while queue non-empty | 0.5879 (sd 0.018) | 0.5978 (sd 0.018) | 0.6029 (sd 0.018) |
| CPU util over makespan | 0.8506 (sd 0.011) | 0.8634 (sd 0.013) | 0.8706 (sd 0.012) |
| fragmentation (frac of busy time) | 0.9237 (sd 0.024) | 0.8451 (sd 0.034) | 0.814 (sd 0.033) |
| queue wait p50 (s) | 18.14 (sd 6.7) | 15.43 (sd 6.9) | 14.14 (sd 6.4) |
| queue wait p99 (s) | 33.39 (sd 9.7) | 28.98 (sd 9.8) | 26.58 (sd 9) |

Paired difference vs **first-fit** (policy minus first-fit, mean +/- 95% CI, wins/losses/ties by seed)
A 'win' is the better direction for that metric. Differences whose CI contains 0 are not distinguishable from noise at this n.

| metric | round-robin | best-fit |
|---|---|---|
| makespan (s) | +4.652 +/- 0.7 (1/49/0) | -2.587 +/- 0.58 (47/3/0) |
| CPU util while queue non-empty | -0.01532 +/- 0.0023 (0/50/0) | +0.008376 +/- 0.0018 (45/5/0) |
| mem util while queue non-empty | -0.009934 +/- 0.0015 (0/50/0) | +0.005086 +/- 0.0012 (44/5/1) |
| CPU util over makespan | -0.01277 +/- 0.0019 (1/49/0) | +0.007164 +/- 0.0016 (47/3/0) |
| fragmentation (frac of busy time) | +0.07861 +/- 0.0095 (0/50/0) | -0.03105 +/- 0.0089 (40/10/0) |
| queue wait p50 (s) | +2.707 +/- 0.51 (2/48/0) | -1.294 +/- 0.39 (43/7/0) |
| queue wait p99 (s) | +4.416 +/- 0.72 (1/49/0) | -2.4 +/- 0.54 (46/4/0) |
