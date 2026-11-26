# Local verification

Run from the repository root on x86-64 Linux or WSL.

| Command | Checks |
|---|---|
| `make check-host` | Engine, model training and loading, language interpreter and compiler, integer matrix kernels, search APIs and static ELF linkage |
| `make check-boot` | BIOS boot at 16, 64 and 320 MiB, timer interrupts, page allocation and mapping, heap reuse, matrix multiplication and guest exit 33 |
| `make test-qkv-gradients` | C11 finite differences for all three attention projections, finite 240-epoch checkpoints, and the second training step on a one-example corpus |
| `make check-local-only` | Rejects workflow YAML in the checkout |
| `make check-memory` | Host C and C++ tests under AddressSanitizer and UndefinedBehaviorSanitizer in a temporary build tree |
| `make check` | Host tests followed by guest boot |

For one guest RAM size run `make -C hadeed runtime-test BOOT_RAM_MB=64`. The guest reports the page count after reserving the first 2 MiB. Physical allocation covers up to 256 MiB. The hosted allocator test also supplies overlapping, disabled, misaligned and out-of-range firmware entries.

Run `make -B -C hadeed EXTRA_CFLAGS=-fcf-protection=branch trace-verify` to check an image with branch protection enabled. The verifier recognizes the four-byte ENDBR64 function entry and checks the PIC and PIT instructions immediately after it. The boot test requires at least three timer interrupts.

To check the assembly trainer on a separate corpus, run `make -C asm test-training TRAINING_DATA=/absolute/path/to/data`. The directory must contain `corpus.txt` and `heldout.txt`, each longer than eight bytes. The target trains for 240 epochs, checks every exported FP32 parameter, and prints the training and held-out losses scaled by 1,000.

The trainer in `model/` reads two Arabic records and exports a checkpoint. The test repeats training with the same seed, CRLF and blank lines, compares both files byte for byte, then maps the checkpoint into the engine. The identity checks accept its SHA-256 and reject changed digests. To check another model of the reference shape as well, set both `NIYAH_MMAP_MODEL_PATH` (absolute path) and `NIYAH_MMAP_MODEL_SHA256`.

Model-file cases cover truncated and trailing bytes, unsupported versions, concurrent readers, and failures during allocation and publication. The failed-save case reopens the previous file and compares its bytes.

The search regressions cover Arabic paths and text, 64 indexes, result metadata larger than 16 KiB, invalid TSV offsets and lengths, read failures and existing output files. The in-memory index tests also exercise tied scores, empty documents, full-length text, and retrying after a failed allocation. HTML extraction tests cover quoted attributes, comments, script and style contents, and link limits.

### Search measurements

Run `make bench-search` to time queries after building a 3,072-document fixture. Each row reports the median of 11 warm batches in nanoseconds per query. Timing excludes process startup, index construction, shard verification and output. The in-memory API sums matching query terms. The default shard search requires all distinct query terms. The optional any-word search sums BM25 contributions for the terms present in each document. Its regression test compares both modes with an exhaustive per-document scorer at six result limits.

For a result comparison, build the same harness against both revisions and run each binary with `--dump`. Use `cmp` on the two outputs. The dump contains document IDs, hexadecimal floating-point scores and the leading source receipt at six result limits. Arabic token-boundary changes and malformed UTF-8 have separate regression cases. The fixture measures latency and result parity. Relevance evaluation requires queries with independently judged documents.

The in-memory index uses 16-slot hash groups to look up terms and document IDs. Seven hash bits filter candidate keys before the full comparison. The scorer accumulates postings in windows of 256 document ordinals and visits only occupied windows. The public in-memory index structure has grown. Recompile applications that allocate it. Existing version 2 shard files use the same on-disk format.

On Windows, `retrieval/ui/build.ps1` builds the native search DLL, the desktop client and the command-line probe. Model weights and generated indexes are local test outputs.

### Batch matching and document fingerprints

The batch index has three banks of 1,024 rows. A rank-zero word covers 64 documents. The next two ranks fold two and four words by OR, preserving the bit position within each word. Every term uses rank zero. Terms present in at most one eighth of the documents also use rank one. Terms present in at most one sixty-fourth use all three. The number of words examined grows with the shard. Each surviving document must pass exact posting membership for every query term.

To compare the two paths, run `batch-match` with and without `--rows` on the same input file and compare stdout with `cmp`. Stderr reports setup and batch time separately. The batch time includes parsing and writing document IDs. The row path also reports the number of candidate IDs before verification.

SimHash uses every indexed term, including Arabic surface forms and stems. Each feature has integer weight `(1 + floor(log2(tf))) * (1 + floor(log2(N / df)))`. A positive vote sets the bit. A tie clears it. Four 16-bit bands select candidate pairs. The Hamming check uses POPCNT on x86-64 CPUs that support it. The portable path stops counting once the distance exceeds the requested radius. The export records the index digest because document frequencies depend on the shard. A distance threshold requires evaluation on labeled examples before use for deduplication.

The C11 regression constructs rank-row collisions, exercises the 64- and 128-document boundaries, and compares the exact IDs against a document-by-term table. A separate exhaustive pair comparison checks each radius from 0 through 3. Source tests cover Arabic text across a read-buffer boundary and a source changed after the quoted bytes.
