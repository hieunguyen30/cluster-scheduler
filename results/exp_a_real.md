results/exp_a_real.txt: 8 seeds per policy

| metric | round-robin | first-fit | best-fit |
|---|---|---|---|
| makespan_s | 16.97 (sd 1.7) | 17.01 (sd 1.9) | 16.42 (sd 1.9) |
| cpu_util_busy | 0.8711 (sd 0.025) | 0.9001 (sd 0.014) | 0.9068 (sd 0.02) |
| mem_util_busy | 0.5846 (sd 0.04) | 0.599 (sd 0.04) | 0.602 (sd 0.041) |
| cpu_util_makespan | 0.7338 (sd 0.023) | 0.7444 (sd 0.019) | 0.7504 (sd 0.019) |
| frag_fraction_busy | 0.9447 (sd 0.047) | 0.835 (sd 0.1) | 0.8313 (sd 0.076) |
| queue_wait_p50_s | 1.183 (sd 0.69) | 0.9975 (sd 0.66) | 0.9277 (sd 0.67) |
| queue_wait_p99_s | 2.933 (sd 1) | 2.65 (sd 1.4) | 2.681 (sd 1.5) |
| retried | 0 (sd 0) | 0 (sd 0) | 0 (sd 0) |
| unfinished | 0 (sd 0) | 0 (sd 0) | 0 (sd 0) |

Paired difference vs first-fit (mean, min..max over seeds; n is small, no CI claimed)

| metric | round-robin | best-fit |
|---|---|---|
| makespan_s | -0.04575 (-1.41..+1.54) | -0.5965 (-1.62..+1.33) |
| cpu_util_busy | -0.02904 (-0.0634..+0.0066) | +0.006637 (-0.0156..+0.0277) |
| mem_util_busy | -0.0144 (-0.0341..+0.0067) | +0.002962 (-0.0105..+0.0176) |
| cpu_util_makespan | -0.01057 (-0.0257..+0.0136) | +0.005987 (-0.0067..+0.0145) |
| frag_fraction_busy | +0.1098 (-0.0376..+0.238) | -0.003675 (-0.105..+0.116) |
| queue_wait_p50_s | +0.1853 (+0.044..+0.427) | -0.06975 (-0.309..+0.108) |
| queue_wait_p99_s | +0.2829 (-0.735..+1.41) | +0.03137 (-1.16..+1.13) |
