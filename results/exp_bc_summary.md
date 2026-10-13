### Experiment B: kill K worker nodes (agent + jobs), 24 jobs of 12 s, 9 workers

mean (min..max) over repetitions

| K killed | reps | jobs lost per run | first DEAD after kill (s) | last DEAD after kill (s) | all lost jobs running again (s) | executions (24 jobs) | all 24 finished |
|---|---|---|---|---|---|---|---|
| 1 | 5 | 4.00 (4.00..4.00) | 4.61 (4.53..4.73) | 4.61 (4.53..4.73) | 6.61 (6.50..6.74) | 28.00 (28.00..28.00) | 5/5 |
| 2 | 5 | 8.80 (8.00..12.00) | 5.05 (4.54..5.84) | 5.06 (4.54..5.85) | 7.14 (6.60..7.96) | 32.80 (32.00..36.00) | 5/5 |
| 3 | 5 | 15.20 (12.00..16.00) | 5.12 (4.56..5.95) | 5.13 (4.57..5.96) | 7.62 (6.67..8.36) | 39.20 (36.00..40.00) | 5/5 |

### Experiment B2: frozen agent (SIGSTOP) then healed (SIGCONT), 12 jobs of 40 s

| reps | jobs on frozen worker | frozen -> DEAD (s) | jobs running twice at once | zombie processes alive before heal | heal -> zombies killed (s) |
|---|---|---|---|---|---|
| 5 | 4.00 (4.00..4.00) | 4.80 (4.75..4.91) | 4.00 (4.00..4.00) | 4.00 (4.00..4.00) | 0.06 (0.06..0.06) |

### Experiment C: SIGKILL the control plane, restart from the WAL (40 jobs of 10 s, 9 workers)

| reps | WAL size (bytes) | restart -> serving (s) | restart -> all 9 workers ALIVE (s) | jobs lost | jobs executed more than once | jobs with attempts>1 |
|---|---|---|---|---|---|---|
| 5 | 12865.00 (12865.00..12865.00) | 0.02 (0.02..0.02) | 0.19 (0.10..0.36) | [0, 0, 0, 0, 0] | [0, 0, 0, 0, 0] | [0, 0, 0, 0, 0] |
