# Local verification

Run from the repository root on x86-64 Linux or WSL.

| Command | Checks |
|---|---|
| `make check-host` | Engine, model training and loading, language interpreter and compiler, integer matrix kernels, search APIs and static ELF linkage |
| `make check-boot` | BIOS image in QEMU, page allocation, matrix multiplication and the guest exit code |
| `make check-memory` | Host C and C++ tests under AddressSanitizer and UndefinedBehaviorSanitizer in a temporary build tree |
| `make check` | Host tests followed by guest boot |

The trainer reads two Arabic records and exports a checkpoint. The test repeats training with the same seed, CRLF and blank lines, compares both files byte for byte, then maps the checkpoint into the engine. The identity checks accept its SHA-256 and reject changed digests. To check another model of the reference shape as well, set both `NIYAH_MMAP_MODEL_PATH` (absolute path) and `NIYAH_MMAP_MODEL_SHA256`.

Model-file cases cover truncated and trailing bytes, unsupported versions, concurrent readers, and failures during allocation and publication. The failed-save case reopens the previous file and compares its bytes.

The search regressions cover Arabic paths and text, 64 indexes, result metadata larger than 16 KiB, invalid TSV offsets and lengths, read failures and existing output files. The in-memory index tests also exercise tied scores, empty documents, full-length text, and retrying after a failed allocation. HTML extraction tests cover quoted attributes, comments, script and style contents, and link limits.

On Windows, `retrieval/ui/build.ps1` builds the native search DLL, the desktop client and the command-line probe. Model weights and generated indexes are local test outputs.
