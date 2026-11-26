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

To check the assembly trainer on a separate corpus, run `make -C asm test-training TRAINING_DATA=/absolute/path/to/data`. The directory must contain `corpus.txt` and `heldout.txt`, each longer than eight bytes. The target trains for 240 epochs, checks every exported FP32 parameter, and prints the training and held-out losses scaled by 1,000.

The trainer in `model/` reads two Arabic records and exports a checkpoint. The test repeats training with the same seed, CRLF and blank lines, compares both files byte for byte, then maps the checkpoint into the engine. The identity checks accept its SHA-256 and reject changed digests. To check another model of the reference shape as well, set both `NIYAH_MMAP_MODEL_PATH` (absolute path) and `NIYAH_MMAP_MODEL_SHA256`.

Model-file cases cover truncated and trailing bytes, unsupported versions, concurrent readers, and failures during allocation and publication. The failed-save case reopens the previous file and compares its bytes.

The search regressions cover Arabic paths and text, 64 indexes, result metadata larger than 16 KiB, invalid TSV offsets and lengths, read failures and existing output files. The in-memory index tests also exercise tied scores, empty documents, full-length text, and retrying after a failed allocation. HTML extraction tests cover quoted attributes, comments, script and style contents, and link limits.

On Windows, `retrieval/ui/build.ps1` builds the native search DLL, the desktop client and the command-line probe. Model weights and generated indexes are local test outputs.
