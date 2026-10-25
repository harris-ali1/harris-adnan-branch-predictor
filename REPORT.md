# Assignment 1 Report: Branch Predictor Implementation

Harris Adnan

## What I did

For the main part of the assignment I implemented `pm_predictor` in `src/my_predictor.h`. It's a simplified version of the Pentium M outcome predictor from the diagram in the handout. It has two parts:

**Bimodal table.** 4096 2-bit saturating counters indexed by the lower 12 bits of the branch address (IP[11:0]). All counters start at 1 (weakly not taken).

**Global predictor.** 512 sets, 4 ways. Each entry has a valid bit, a 6-bit tag, a 2-bit counter and LRU bits. Since we were told we could skip the PIR, I used a 15-bit global history register (GHR) of the last 15 conditional branch outcomes instead and XORed it with the address, like gshare:

- HASH = IP[14:0] XOR GHR
- index = HASH[14:6] (9 bits, 512 sets)
- tag = HASH[5:0] (6 bits)

**How a prediction is made.** If the global predictor has a matching tag in the set (a hit), I use its counter. If not, I fall back to the bimodal counter.

**How the update works.**
- The bimodal counter always gets updated.
- On a global hit, that entry's counter is updated and it becomes most recently used.
- On a global miss, I only allocate a new entry (replacing the LRU way) if the bimodal predictor got the branch wrong. The new counter starts at 2 if the branch was taken and 1 if not.
- The GHR only shifts in conditional branch outcomes. Other branches are just predicted taken and don't change anything.

I went with "allocate only when bimodal is wrong" instead of "allocate on every miss" like the worked example in the handout does, because it did a lot better in my tests (see the experiments section). My reasoning is that if the bimodal table already gets a branch right, it doesn't need to take up space in the global table.

The handout said 2-way was OK, but I did 4-way since that's what the real Pentium M has and it turned out better. 2-way can be selected with `-DPM_GLOBAL_WAYS=2`.

### Extra credit

I also tried the extra credit (`cpm_predictor`), which is the full Pentium M unit from the diagram. On top of the bimodal and global parts it has:

- A BTB (512 sets x 4 ways, index IP[12:4], tag IP[21:13] plus offset IP[3:0]) that stores the branch type and target.
- A loop predictor (64 sets x 2 ways, index IP[9:4], tag IP[15:10], with count, limit and a direction bit).
- An iBTB for indirect branches (256 entries, indexed by the hash).
- A 15-bit PIR. I couldn't find the exact update rule, so mine shifts left by 2 and XORs in IP[18:4] on every taken branch. This is my best guess.
- A small return address stack. It's not in the diagram, but the paper says the Pentium M has one.

The loop predictor algorithm isn't described in the handout, so I made up my own. It counts how many times the branch goes the "loop body" direction, remembers that count as the limit when the loop exits, and predicts the exit when the count reaches the limit. I only trust it after it sees the same trip count twice in a row.

For target prediction: if the branch is predicted taken and hits in the BTB, I use the BTB target (or the iBTB/RAS for indirect branches and returns). If it's predicted not taken, the target is the fall-through address. Conditional jumps in the traces are either 2 or 6 bytes long, so I keep a small table that remembers which length each branch is.

### Bugs I found in the provided code

1. In `predictor.h`, `target_prediction()` returns `bool` instead of `unsigned int`. Every target got turned into 0 or 1, so basically every target prediction counted as wrong. I changed the return type to `unsigned int`. This doesn't affect the normal `predict` program since it only measures direction.
2. The `run_extra` script doesn't work. `predict_extra_credit` prints two numbers ("Direction: x MPKI; Target: y MPKI") but the script was written for one number, so it crashes with dc errors. I rewrote it to pull out both numbers and print both averages.

## Results

I ran all 20 traces. Numbers are mispredictions per 1000 instructions (MPKI), so lower is better.

| Benchmark | Bimodal only | gshare (given) | PM 2-way | PM 4-way (mine) | Extra credit (dir) | Extra credit (target) |
|---|---|---|---|---|---|---|
| 164.gzip | 15.030 | 12.473 | 12.520 | 12.436 | 12.726 | 12.829 |
| 175.vpr | 14.671 | 13.415 | 13.866 | 13.298 | 13.166 | 13.195 |
| 176.gcc | 19.347 | 11.254 | 10.618 | 9.285 | 9.924 | 11.259 |
| 181.mcf | 33.245 | 15.837 | 16.398 | 15.621 | 16.726 | 16.767 |
| 186.crafty | 13.075 | 5.837 | 5.983 | 5.211 | 5.370 | 5.752 |
| 197.parser | 15.417 | 10.008 | 9.036 | 8.538 | 9.083 | 9.148 |
| 201.compress | 9.356 | 7.831 | 7.854 | 7.702 | 8.025 | 8.071 |
| 202.jess | 8.809 | 1.562 | 1.149 | 1.118 | 1.043 | 1.385 |
| 205.raytrace | 3.806 | 2.756 | 2.503 | 2.307 | 2.400 | 3.015 |
| 209.db | 6.480 | 3.909 | 3.723 | 3.615 | 3.623 | 4.401 |
| 213.javac | 3.181 | 2.267 | 2.126 | 2.037 | 1.952 | 2.309 |
| 222.mpegaudio | 3.154 | 2.188 | 2.124 | 2.028 | 1.944 | 2.267 |
| 227.mtrt | 3.706 | 2.657 | 2.470 | 2.383 | 2.318 | 2.798 |
| 228.jack | 7.691 | 3.033 | 2.420 | 2.353 | 2.373 | 2.961 |
| 252.eon | 9.405 | 1.807 | 1.501 | 1.425 | 2.868 | 3.057 |
| 253.perlbmk | 5.955 | 2.554 | 1.917 | 1.819 | 2.537 | 2.707 |
| 254.gap | 9.675 | 3.926 | 3.604 | 3.507 | 3.792 | 3.864 |
| 255.vortex | 1.916 | 1.222 | 0.854 | 0.828 | 1.157 | 1.903 |
| 256.bzip2 | 0.112 | 0.094 | 0.059 | 0.057 | 0.052 | 0.122 |
| 300.twolf | 21.942 | 21.489 | 21.981 | 21.389 | 20.627 | 20.533 |
| **Average** | **10.299** | **6.306** | **6.135** | **5.848** | **6.085** | **6.417** |

For reference, the original stub (always predict taken) gets 86.312 MPKI on average.

My 4-way PM predictor gets **5.848 MPKI**, which beats the gshare that came with the code (6.306). It also uses less memory. gshare has 32K 2-bit counters (64 Kbits). Mine has 8 Kbits for the bimodal table plus about 22 Kbits for the global table (2048 entries x 11 bits), so around 30 Kbits total.

## Experiments

I tried a few variations to see what mattered (averages over all 20 traces):

| Variation | Avg MPKI |
|---|---|
| 2-way, allocate on every miss | 7.259 |
| 2-way, allocate only when bimodal is wrong | 6.135 |
| 4-way, allocate on every miss | 6.605 |
| 4-way, allocate only when bimodal is wrong (final) | 5.848 |
| 8-way | 5.756 |
| 4-way, 8-bit history | 7.517 |
| 4-way, 12-bit history | 6.457 |
| 4-way, 30-bit history folded into 15 | 6.211 |

What I noticed:

- **The allocation policy made the biggest difference.** Allocating on every miss fills the global table with branches the bimodal table already handles, which kicks out the ones that actually need history.
- **More ways helps, but less each time.** Going from 2 to 4 ways helped noticeably; 4 to 8 helped only a little.
- **15 bits of history worked best.** Fewer bits loses information. More bits folded into 15 just causes more collisions, since the tag is only 6 bits.
- **twolf and mcf stay bad no matter what (over 15 MPKI).** Their branches probably depend on data rather than on patterns in the history.

For the extra credit version:

- **The full version did slightly worse than my simpler required version** (6.085 vs 5.848). Turning off the loop predictor gave 6.006, and replacing my PIR with the plain GHR gave 5.937.
- **The best combination I found was 5.778:** the GHR instead of the PIR, and the loop predictor overriding the global predictor (the diagram has it the other way around). The loop predictor can only help when it's allowed to win.
- **My PIR guess probably isn't the real Pentium M rule.** It especially hurt on eon and perlbmk.
- **Target MPKI is only about 0.33 higher than direction MPKI.** So once the direction is right, the BTB almost always has the right target.

## References

- Uzelac and Milenkovic, "Experiment Flows and Microbenchmarks for Reverse Engineering of Branch Predictor Structures," ISPASS 2009 (the diagram in the handout)
- Milenkovic, Milenkovic and Kulick, "Demystifying Intel Branch Predictors," WDDD 2002
- CBP-2 infrastructure by Daniel Jiménez
- I didn't use outside cache code. The set-associative tables and LRU are written directly in `my_predictor.h`.
- I used Claude (generative AI) to help write the code and this report, which was allowed for this assignment.