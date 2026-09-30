# Benchmark results

Measured locally on **September 30, 2026**. These are observations from one machine and workload, not service-level guarantees. The original resume wording used “TPS”; this engine exposes individual key operations, so **ops/s** is the accurate unit.

## Environment

- Apple M3 Pro, 18 GiB RAM, macOS 27.0, arm64.
- Apple Clang 21.0.0, C++17, `-O3 -DNDEBUG -pthread`.
- One benchmark thread; fresh local temporary database for each run.
- 100,000 keys, 16-byte keys, 128-byte values.
- 4 MiB flush threshold, compaction at 4 tables.
- 1,000,000 operations in each random-read and mixed phase.
- Three sequential runs per durability mode, using identical sizes and seed `42`.

The [recorded environment](../bench/results/environment.txt), [run parameters](../bench/results/buffered-1.txt), and all [raw CSV results](../bench/results/) are committed. Runs used a normal desktop session without CPU pinning or exclusive machine access. Storage hardware model and physical power-loss behavior were not measured.

## Results

Each cell is the median of the three runs for that metric. Percentiles are calculated within each run, then summarized; these are not pooled percentiles. Maximum latency is the worst sample across all three runs. No outliers were removed.

| Mode | Workload | Median ops/s | P50 μs | P95 μs | P99 μs | Worst max μs |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Buffered | Sequential put | 409,636 | 1.917 | 2.833 | 4.833 | 13,801.125 |
| Buffered | Random read hit, warm SSTable | 2,612,196 | 0.334 | 0.625 | 0.791 | 1,133.584 |
| Buffered | Random read miss, warm SSTable | 2,993,822 | 0.292 | 0.583 | 0.750 | 24.958 |
| Buffered | 90% reads / 10% writes | 1,037,220 | 0.625 | 2.416 | 2.792 | 82,106.750 |
| Sync | Sequential put | 34,585 | 21.000 | 27.458 | 34.625 | 754,426.334 |
| Sync | Random read hit, warm SSTable | 2,734,486 | 0.333 | 0.583 | 0.709 | 28.625 |
| Sync | Random read miss, warm SSTable | 3,053,558 | 0.292 | 0.542 | 0.708 | 25.625 |
| Sync | 90% reads / 10% writes | 333,837 | 0.791 | 21.166 | 22.917 | 74,545.709 |

The mixed P99 includes both reads and writes. It is not a read-only latency statistic. The “Sync” label on read phases describes how their dataset was loaded; a read itself does not call `fsync`.

### What the numbers support

- Buffered sequential writes exceeded 100K ops/s in every measured run (403K–411K).
- Warm-cache random read-hit P99 was below 10 μs in every measured run (0.667–1.291 μs across both modes).
- Synchronous write throughput was 33K–45K ops/s. These results do **not** support a claim of 100K individually synced writes/s.
- Full merges produce visible tail stalls: the worst buffered mixed operation took about 82 ms. A low P99 does not imply a bounded maximum latency.
- The dataset fits in RAM. These results do not characterize cold-cache reads, datasets larger than memory, concurrent-client throughput, network latency, or long-duration steady state.

## Methodology

[`benchmark.cpp`](../bench/benchmark.cpp) pre-generates keys and uniform random request indices to keep key formatting and RNG generation outside measured loops. Existing keys use even numeric suffixes; missing keys use interleaved odd suffixes, so misses search the same key range.

1. **Sequential put:** insert all 100,000 keys in sorted order. Include automatic flushes/compactions and a final WAL `sync()` in total throughput time.
2. **Preparation:** explicitly compact to one SSTable, then read every key and verify its value. Preparation is outside timed phases. This warms the mapped table pages and leaves the memtable empty.
3. **Read hits:** perform 1,000,000 uniformly distributed reads and verify every returned value.
4. **Read misses:** perform 1,000,000 reads of interleaved absent keys and verify every miss.
5. **Mixed:** issue 1,000,000 operations, every tenth operation a put of the same 128-byte value and the other nine reads. Keys follow the pre-generated random sequence. Automatic flushes/compactions occur inside this phase; a final WAL sync is included in total throughput time.

Every operation is timed with `std::chrono::steady_clock`; P50/P95/P99 use the nearest-rank definition. Latency samples include verification, clock overhead, and any synchronous maintenance performed by that operation. Total throughput also includes timer/sample-storage overhead and, in write phases, the final explicit sync. That final sync is not assigned to an individual operation's latency sample. Sample sorting and CSV formatting are outside timed regions.

Both durability modes append to the WAL with `write`. Sync mode additionally calls POSIX `fsync` after every mutation. Buffered mode returns without that per-operation persistence barrier; checkpointing and the final explicit sync still issue barriers. On macOS, `fsync` is not `F_FULLFSYNC`; see the [persistence limits](architecture.md#recovery).

## Reproduce

```sh
make -j
mkdir -p build/results
for mode in buffered sync; do
  for run in 1 2 3; do
    ./build/kv_bench 100000 1000000 "$mode" \
      > "build/results/$mode-$run.csv" \
      2> "build/results/$mode-$run.txt"
  done
done
```

The benchmark removes its generated temporary database after each run. Timings will change with hardware, filesystem, compiler, background activity, workload size, and durability settings. GitHub Actions runs a small benchmark only as a functional smoke test; it does not enforce hardware-dependent performance thresholds.

## Resume wording

> Built a persistent C++17 key-value store using an LSM tree and checksummed WAL; benchmarked 100K+ buffered writes/s and under 10 μs P99 warm-cache reads on an Apple M3 Pro.

For a more specific version tied to the committed three-run medians:

> Built a C++17 LSM key-value store with WAL recovery and compaction, achieving 410K buffered writes/s and 0.79 μs P99 warm-cache reads on a 100K-key benchmark.

Keep the workload and buffered-write qualifier when discussing these results. They describe separate write and read benchmark phases.
