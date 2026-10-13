results/exp_a_sim_load0.85.csv: 50 seeds, workers=9, jobs=2000, load=0.850

| metric | round-robin | first-fit | best-fit |
|---|---|---|---|
| makespan (s) | 333.4 (sd 5.5) | 332.3 (sd 5.3) | 331.9 (sd 5.3) |
| CPU util while queue non-empty | 0.877 (sd 0.014) | 0.8983 (sd 0.012) | 0.9116 (sd 0.011) |
| mem util while queue non-empty | 0.5651 (sd 0.019) | 0.5803 (sd 0.022) | 0.5849 (sd 0.024) |
| CPU util over makespan | 0.8065 (sd 0.029) | 0.8093 (sd 0.03) | 0.8102 (sd 0.03) |
| fragmentation (frac of busy time) | 0.9454 (sd 0.026) | 0.8661 (sd 0.042) | 0.8274 (sd 0.047) |
| queue wait p50 (s) | 1.842 (sd 2.4) | 0.88 (sd 1.5) | 0.5803 (sd 1.4) |
| queue wait p99 (s) | 7.587 (sd 3.4) | 6.225 (sd 2.8) | 5.564 (sd 2.7) |

Paired difference vs **first-fit** (policy minus first-fit, mean +/- 95% CI, wins/losses/ties by seed)
A 'win' is the better direction for that metric. Differences whose CI contains 0 are not distinguishable from noise at this n.

| metric | round-robin | best-fit |
|---|---|---|
| makespan (s) | +1.148 +/- 0.49 (1/33/16) | -0.3864 +/- 0.21 (21/4/25) |
| CPU util while queue non-empty | -0.0213 +/- 0.0028 (1/49/0) | +0.01332 +/- 0.0022 (48/2/0) |
| mem util while queue non-empty | -0.01513 +/- 0.0033 (2/48/0) | +0.004642 +/- 0.0015 (39/9/2) |
| CPU util over makespan | -0.002832 +/- 0.0013 (1/33/16) | +0.000928 +/- 0.00051 (21/4/25) |
| fragmentation (frac of busy time) | +0.07931 +/- 0.011 (0/50/0) | -0.03865 +/- 0.011 (41/9/0) |
| queue wait p50 (s) | +0.9624 +/- 0.32 (0/45/5) | -0.2997 +/- 0.11 (31/1/18) |
| queue wait p99 (s) | +1.361 +/- 0.38 (5/45/0) | -0.6612 +/- 0.2 (40/10/0) |
