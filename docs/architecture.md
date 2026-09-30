# Architecture and recovery

## Write path

1. Validate key/value lengths and acquire the exclusive store mutex.
2. Append one checksummed record to `WAL`, retrying interrupted or partial writes.
3. In `Sync` mode, `fsync` the WAL before making the operation visible.
4. Update the ordered `std::map` memtable; deletes store a tombstone.
5. If memtable payload bytes or WAL bytes reach `memtable_bytes`, checkpoint the memtable. Including WAL bytes prevents repeated updates to one key from growing the log indefinitely.
6. If the number of tables reaches `max_tables`, merge all tables.

The exclusive mutex makes a successful operation visible atomically to readers sharing the same instance. It also serializes checkpoints and compaction. There are no multi-key transactions. If an I/O error occurs after a WAL append, the operation's outcome can be uncertain: reopen and inspect the key. The instance rejects subsequent operations after a mutation/checkpoint failure rather than continuing from possibly inconsistent state.

## Read path

Readers share a `std::shared_mutex`. A read searches the memtable first, then searches each table from newest to oldest. Each table has an immutable vector of record views pointing into its memory mapping. `std::lower_bound` locates the requested key in that vector. The first matching value or tombstone wins. A returned value is copied into an owning `std::string`, so it remains valid after compaction or store destruction.

For `T` tables with at most `N` entries per table, a disk-table lookup performs up to `O(T log N)` key comparisons. Reads use the OS page cache; warm-cache benchmark timings do not describe cold storage latency. There is no separate application value cache or Bloom filter.

## Checkpoint ordering

The store directory contains `LOCK`, `WAL`, `MANIFEST`, and `sst-ID.kv` files. A store holds an advisory exclusive `flock` on `LOCK` for its entire lifetime. All users of this format must respect the lock.

Checkpoint publication follows this order:

1. Create a new table with a fresh ID using `O_EXCL`.
2. Write the sorted memtable, then sync the table and its directory.
3. Write and sync `MANIFEST.tmp` with the old table IDs plus the new ID.
4. Atomically rename it to `MANIFEST`, then sync the directory.
5. Truncate and sync the WAL.
6. Clear the memtable.

| Crash point | Reopen behavior |
| --- | --- |
| Before manifest publication | Use the old table set and recover writes from the WAL. Ignore the unfinished/unreferenced table. |
| After publication, before WAL truncation | Load the new table and replay duplicate writes. Reapplying puts/deletes is idempotent. |
| After durable WAL truncation | Load the published table; the empty WAL needs no replay. |

Compaction first checkpoints the current memtable. It processes all tables oldest to newest, replacing earlier values and removing keys with tombstones. It then publishes one merged table through the same manifest protocol. Only after publication does it unlink old tables. Dropping tombstones is safe because the merge includes **every** table and the WAL checkpoint has completed.

The current compactor materializes the merged live dataset in a `std::map`. This keeps the implementation small but costs `O(live data)` extra memory, rewrites live data on each full merge, and blocks readers/writers. A streaming merge and background leveled compaction would be the next changes for larger datasets or tighter tail-latency requirements.

## Recovery

At open, the store locks the directory, validates the manifest, maps and checks referenced tables, and replays the WAL in order. An incomplete final WAL record is truncated to the last complete record. A complete record with a bad header/body checksum is an error; it is not silently skipped. The header checksum protects lengths before they are used to identify a torn payload.

Table IDs are allocated above all existing table filenames, including orphans, so recovery never overwrites a file left by an interrupted write. Temporary manifests and unreferenced tables are ignored when a valid manifest exists. They may consume disk space after repeated failures. A missing manifest in a directory containing data is rejected to avoid silently discarding tables.

`fsync` and atomic rename provide the filesystem ordering used here. Device caches and platform-specific persistence guarantees still apply; in particular, this implementation does not issue macOS `F_FULLFSYNC`. `SIGKILL` tests leave the kernel and device running and therefore do not certify physical power-loss behavior.

## Version 1 file formats

All integers are unsigned and little-endian. Strings are raw bytes with explicit lengths. Checksums use 32-bit FNV-1a, starting at `2166136261` and multiplying by `16777619` after each byte XOR. They detect accidental damage; they are not authentication or cryptographic integrity checks.

### WAL record (21-byte header + payload)

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | Magic `KVR1` |
| 4 | 1 | Type: `1` put, `2` delete |
| 5 | 4 | Key length, at most 1 MiB |
| 9 | 4 | Value length, at most 16 MiB; zero for delete |
| 13 | 4 | Checksum of bytes 0–12 |
| 17 | 4 | Checksum of the concatenated key and value |
| 21 | variable | Key followed by value |

`WAL` is a sequence of these records. A zero-length put value differs from a delete because the type is explicit.

### Sorted table

`KVS1` magic (4 bytes), record count (8 bytes), then that many WAL-format records sorted by unique key. The count and exact file length prevent truncation at a record boundary from being accepted as a complete table. Opening validates every record, key order, and the absence of trailing bytes.

### Manifest

`KVM1` magic (4 bytes), table count (4 bytes), table IDs (8 bytes each, oldest to newest), then a 4-byte checksum of all preceding bytes. IDs must be nonzero and unique; every referenced table must exist and validate. Version 1 has no migration tooling; incompatible format versions fail to open.
