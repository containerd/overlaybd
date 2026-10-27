# OverlayBD Writable Layer Random Read/Write Performance Reference

## Test Environment
  - instance: ecs.ebmc8i.48xlarge
  - cpu: Intel(R) Xeon(R) Platinum 8475B
  - mem: 512G
  - disk: ESSD-PL3 (8TB / ~411K iops)

## Test Method
  Each test creates an overlaybd-ublk device with a single hardware queue and nr_tags=64:
  - Random read/write: write 20GB of data from /dev/urandom, then run fio 4K random read/write over the 20GB with iodepth 1/16/32/64
  - Sequential write: dd bs=1M count=20480 oflag=direct conv=fdatasync

## 1. 4KiB Random Read (IOPS / MiB/s)

| Format / backend | QD1 | QD16 | QD32 | QD64 |
| --- | --- | --- | --- | --- |
| hybrid / psync | 82,947.82 / 324.01 | 247,707.47 / 967.61 | 248,127.44 / 969.25 | 98,027.98 / 382.92 |
| hybrid / libaio | 3,614.52 / 14.12 | 58,845.75 / 229.87 | 88,755.76 / 346.70 | 191,741.59 / 748.99 |
| sparse / psync | 87,504.65 / 341.82 | 245,915.72 / 960.61 | 253,221.48 / 989.15 | 101,678.71 / 397.18 |
| sparse / libaio | 3,558.36 / 13.90 | 57,463.02 / 224.46 | 86,490.90 / 337.86 | 197,229.21 / 770.43 |
| hybrid / uring-buffered | 77,544.82 / 302.91 | 255,570.80 / 998.32 | 266,368.82 / 1,040.50 | 120,676.86 / 471.39 |
| sparse / uring-buffered | 77,390.75 / 302.31 | 256,014.52 / 1,000.06 | 269,523.27 / 1,052.83 | 132,314.91 / 516.86 |
| hybrid / uring-direct | 3,615.16 / 14.12 | 64,548.88 / 252.14 | 128,943.35 / 503.68 | 219,075.51 / 855.76 |
| sparse / uring-direct | 3,195.33 / 12.48 | 65,814.55 / 257.09 | 127,367.74 / 497.53 | 218,252.91 / 852.55 |
| loop / loop-buffered | 124,713.62 / 487.16 | 447,455.29 / 1,747.87 | 473,367.83 / 1,849.09 | 503,095.71 / 1,965.22 |
| loop / loop-direct | 3,736.54 / 14.60 | 67,299.62 / 262.89 | 134,905.23 / 526.97 | 260,913.05 / 1,019.19 |

## 2. 4KiB Random Overwrite (IOPS / MiB/s)

| Format / backend | QD1 | QD16 | QD32 | QD64 |
| --- | --- | --- | --- | --- |
| hybrid / psync | 72,331.64 / 282.55 | 170,147.56 / 664.64 | 174,074.14 / 679.98 | 68,059.98 / 265.86 |
| hybrid / libaio | 6,381.77 / 24.93 | 6,923.67 / 27.05 | 7,086.41 / 27.68 | 6,909.79 / 26.99 |
| sparse / psync | 63,087.00 / 246.43 | 123,807.06 / 483.62 | 130,581.88 / 510.09 | 69,277.69 / 270.62 |
| sparse / libaio | 6,424.34 / 25.10 | 86,831.88 / 339.19 | 143,166.19 / 559.24 | 97,665.43 / 381.51 |
| hybrid / uring-buffered | 58,663.40 / 229.15 | 60,320.66 / 235.63 | 58,511.33 / 228.56 | 58,711.79 / 229.34 |
| sparse / uring-buffered | 51,702.39 / 201.96 | 128,376.73 / 501.47 | 129,108.26 / 504.33 | 67,682.49 / 264.38 |
| hybrid / uring-direct | 6,629.75 / 25.90 | 7,057.69 / 27.57 | 7,163.42 / 27.98 | 6,796.12 / 26.55 |
| sparse / uring-direct | 6,619.58 / 25.86 | 7,157.34 / 27.96 | 6,830.31 / 26.68 | 6,748.68 / 26.36 |
| loop / loop-buffered | 104,043.40 / 406.42 | 299,456.34 / 1,169.75 | 307,276.59 / 1,200.30 | 323,788.26 / 1,264.80 |
| loop / loop-direct | 6,978.56 / 27.26 | 118,525.09 / 462.99 | 238,877.41 / 933.11 | 384,939.97 / 1,503.67 |

## 3. dd 1MiB Sequential First Write / Overwrite

| Format / backend | First write | Overwrite |
| --- | --- | --- |
| hybrid / psync | 1,349.74 MiB/s & 15.17 s | 1,425.30 MiB/s & 14.37 s |
| hybrid / libaio | 547.94 MiB/s & 37.38 s | 579.73 MiB/s & 35.33 s |
| sparse / psync | 1,354.62 MiB/s & 15.12 s | 1,415.29 MiB/s & 14.47 s |
| sparse / libaio | 850.63 MiB/s & 24.08 s | 854.32 MiB/s & 23.97 s |
| hybrid / uring-buffered | 1,286.33 MiB/s & 15.92 s | 1,420.27 MiB/s & 14.42 s |
| sparse / uring-buffered | 1,319.61 MiB/s & 15.52 s | 1,435.20 MiB/s & 14.27 s |
| hybrid / uring-direct | 525.39 MiB/s & 38.98 s | 577.28 MiB/s & 35.48 s |
| sparse / uring-direct | 568.68 MiB/s & 36.01 s | 583.85 MiB/s & 35.08 s |
| loop / loop-buffered | 1,120.96 MiB/s & 18.27 s | 1,099.81 MiB/s & 18.62 s |
| loop / loop-direct | 1,002.88 MiB/s & 20.42 s | 1,030.65 MiB/s & 19.87 s |
