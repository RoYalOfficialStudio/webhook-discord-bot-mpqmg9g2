# A/B benchmark 2026-10-28 (same VM, back to back): 8ca4d5d (before GMB 13) vs final

```
run 1
abo:  MEDIUM | 1 | 52 % | 20 / 3750   MEDIUM | 4 | 29 % | 17 / 3750   LARGE | 1 | 100 % | 1487 / 3750   LARGE | 4 | 38 % | 14 / 3750  
abn:  MEDIUM | 1 | 53 % | 19 / 3750   MEDIUM | 4 | 28 % | 13 / 3750   LARGE | 1 | 99 % | 1430 / 3750   LARGE | 4 | 43 % | 18 / 3750  
run 2
abo:  MEDIUM | 1 | 54 % | 12 / 3750   MEDIUM | 4 | 27 % | 7 / 3750   LARGE | 1 | 103 % | 1684 / 3750   LARGE | 4 | 40 % | 9 / 3750  
abn:  MEDIUM | 1 | 55 % | 31 / 3750   MEDIUM | 4 | 26 % | 1 / 3750   LARGE | 1 | 101 % | 1617 / 3750   LARGE | 4 | 48 % | 36 / 3750  

[exited with code 0]
```

columns: size | threads | CPU mean | blocks over budget. abo = old (8ca4d5d), abn = new.
Result: MEDIUM and LARGE/1 thread equal within noise; LARGE/4 threads +5..8 percentage points in both runs (possible small regression, cause NOT identified; shared cloud VM, no realtime scheduling).
