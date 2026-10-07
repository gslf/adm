# Search performance

Measured on 2026-10-06 with an Intel Core Ultra 7 265U (14 logical CPUs), Linux
7.2.5, glibc 2.44 and GCC 16.2.1, `-std=c11 -O2 -D_DEFAULT_SOURCE -pthread`.
The x86 matcher selected AVX2. Each entry is the median of five warm-cache runs
after an unmeasured warm-up. No cache flushing, CPU pinning or frequency locking
was used. The baseline is a saved source snapshot of adm's search immediately
before this optimization, compiled with the same flags.

## End-to-end measurements

The adm timer includes worker startup, directory traversal, file reads and creation
of the navigable occurrence index. It excludes process startup and pattern
compilation. Results are synthetic-corpus measurements, not universal performance
guarantees. All measured runs verify the expected number of occurrences.

| Corpus/query | Old adm | New, 1 scanner | New, automatic (8) |
|---|---:|---:|---:|
| 128 MiB, 32 files, literal `needle7`, 32 hits | 87.2 ms | 26.5 ms | 5.3 ms |
| Same files, regex `needle[0-9]+`, 32 hits | 2433.3 ms | 25.8 ms | 5.3 ms |
| 128 MiB, one file, literal, 1 hit | 88.5 ms | 25.7 ms | 27.1 ms |
| 8 MiB, 32 files, regex `[0-9]+`, 32 hits | 125.1 ms | 2.7 ms | 0.7 ms |
| 16 MiB, 32 files, literal, 16,384 hits | 11.4 ms | 4.1 ms | 1.2 ms |
| 4000 files of 1 KiB, literal, 4000 hits | 15.5 ms | 14.5 ms | 12.9 ms |

Files contain repeated 1024-byte lines of ASCII words. Sparse fixtures have a
single matching final line per file; dense fixtures match every line. The script
also measures 2, 4 and 8 scanners independently. These fixtures strongly favor
literal and character-class prefilters. Tiny files improve less, and one file is
scanned by one scanner even when the pool has multiple workers.

GNU grep 3.12-modified and ripgrep 15.2.0 were also measured with verified
occurrence counts:

| Corpus | `grep -r -o` | `rg -uuu --count-matches -j8` |
|---|---:|---:|
| Large sparse literal | 19.4 ms | 7.0 ms |
| Regex prefix | 72.4 ms | 7.2 ms |
| Single large file | 17.8 ms | 24.9 ms |
| Regex character class | 4.2 ms | 3.0 ms |
| Dense literal | 1.3 ms | 3.5 ms |
| Small files | 7.0 ms | 7.4 ms |

These are reference points, not equivalent engine comparisons: external timings
include process startup; grep emits every occurrence to `/dev/null`, ripgrep
counts occurrences, and adm creates a row/column/length index on temporary storage.
Synthetic text files have no ignores, symlinks, binary content or `.git` metadata,
so the searched file sets are equivalent. Cold disks, network filesystems,
non-ASCII text, different patterns and very dense output may behave differently.
Regexes without a usable prefix or initial character-class filter still run through
the UTF-8 state machine and may be slower than GNU grep or ripgrep.

## Reproduce

From the project root:

```sh
make bench-search
python3 tests/bench_search.py --mib 128 --repeat 5
```

`--baseline /path/to/old-benchmark-driver` adds an old adm implementation to the
comparison. The driver is `tests/bench_search.c`; compile it against a saved old
source tree with `-DADM_BENCH_BASELINE` to use the earlier search-job API. The corpus
and binaries live in a temporary directory and are removed when the script exits.
No external search executable is needed by adm; the benchmark uses grep/ripgrep
only if installed.

## Implementation and verification

- A traversal producer feeds a bounded queue to native POSIX/Windows scanners.
  Automatic selection uses at most eight scanners; `ADM_SEARCH_THREADS=1..32`
  overrides it. Partial thread-start failure falls back to fewer scanners.
- Literal byte-pair probes use runtime-selected AVX2/SSE2, or AArch64 NEON. Other
  targets use portable `memchr`. Every load stays inside the supplied line.
  Dense false candidates fall back to KMP for linear-time literal matching.
- Mandatory ASCII prefixes and initial character-class byte filters skip impossible
  regex starts. ASCII class membership is compiled once. Non-ASCII bytes remain
  candidates for the existing UTF-8 engine, preserving malformed-byte behavior.
- In-chunk lines are scanned directly; only lines crossing read chunks need copying.
  Workers keep bounded hit batches in memory and spill larger sets to disk.
- Per-file validation precedes parallel publication. A separate publishing mutex
  preserves contiguous, ordered file results while short index batches let the TUI
  read progress. Binary or unstable files are discarded locally. Cancellation joins
  scanners before releasing their job; queued paths and temporary indexes are freed.
- Current-buffer scanning and workspace replacement remain serial. Immutable buffer
  borrowing, shared-document ownership, stale-file checks and atomic file writes
  retain their existing guarantees.

Regression coverage includes scalar/SIMD comparisons against a naive literal
matcher, filtered/unfiltered UTF-8 regex comparisons, POSIX regex comparisons,
protected-page boundaries, repetitive text, parallel counts and index ordering,
late binary detection, long lines, dense results spilling to disk, repeated queue
cancellation, shared-buffer edits and terminal navigation. ASan/UBSan and
ThreadSanitizer were run on Linux. ASan leak detection was disabled because
LeakSanitizer is unavailable under this environment's tracing. Windows and AArch64 paths were implemented but
were not runtime-tested on this machine.

The implementation takes useful ideas from [GNU grep's performance documentation](https://www.gnu.org/software/grep/manual/html_node/Performance.html),
[ripgrep's source and design](https://github.com/BurntSushi/ripgrep), and the
[author's explanation of literal filters, parallel file scans and buffering](https://burntsushi.net/ripgrep/).
