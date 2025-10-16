# Lookup Algorithm in LSMT

## Description

LBA lookup in LSMT can be abstracted as a segment search problem, searching within a sorted set of non-overlapping intervals. Previously, we used binary search via std::lower_bound. Now, we've adopted a linearized B+ tree combined with AVX-512, which better exploits CPU cache efficiency and delivers over a 10X speedup in lookup performance. Even in environments without AVX-512 support, using a loop optimized with bitmask still yields significant performance gains.


## Performance

| segment count | b+tree + avx512 | b+tree + loop + bitmask | lower bound |
|---------------|-----------------|---------------|-------------|
| 1k   | 220 M/s | 42.2 M/s | 18.3 M/s |
| 10k  | 160 M/s | 30.7 M/s | 12.8 M/s |
| 100k | 108 M/s | 21.8 M/s | 8.6 M/s  |
| 1M   | 57.4 M/s | 15.2 M/s | 5.6 M/s  |