# Architecture

| Path | Executed behavior | Execution environment |
|---|---|---|
| `engine/` | CPU FP32 transformer, optimizer, tokenizer, checkpoints, training, evaluation and decoding | Hosted C11 on Linux in the default build |
| `lang/` | Scalar lexer, parser, checker, interpreter and ELF emitter. The separate tensor frontend emits a typed instruction stream | Linux x86-64 |
| `asm/` | Interval allocation, tensor execution, byte language models, exact fractions, candidate verification, morphology codec and rank-two adapter tool | Linux x86-64 syscall ABI |
| `hadeed/` | BIOS loader, long-mode entry, E820 discovery, buddy allocation, heap and signed integer matrix self-test | Freestanding x86-64 |
| `model/` | Raw FP32 v5 weights, GGUF tensor access, local training and inference | Host |
| `retrieval/` | Document indexing, ranked search, and structured source receipts | Host CLI, C API and Windows desktop client |

## Model files

The model trainer writes a 64-byte header followed by dense FP32 weights. The reader checks the layout and payload length before accepting a regular file. A save stages a replacement in the destination directory so readers can continue to open the previous complete file until the new one is ready. Engine training checkpoints and GGUF containers have separate formats.

## Local search

The index builder writes a `.dalw` file containing terms, posting lists, document metadata, and a SHA-256 trailer. Queries intersect posting lists and rank the matching documents. A result receipt identifies the shard and document and records the original source location. The CLI writes one receipt per line. The desktop client calls the native search API through a local DLL.

## Tensor execution

The tensor frontend parses `asm/examples/tensor_arabic.niyah` using the lexing and morphology code in `lang/src`. It writes typed IR. The allocator reads its live intervals and assigns storage. The emitter consumes the IR and assignment, and GNU binutils link the generated program with the runtime and dispatch code. The same morphology implementation also serves the standalone surface-token codec. A compile-time ABI check covers structure sizes, offsets and enum values used by assembly.

The program reads a row-major 16×64 matrix followed by a row-major 64×16 matrix. Inputs are signed 8-bit integers. The output consists of 256 signed 32-bit little-endian values in row order. Partial and trailing input produce exit 65 before output. The scalar kernel accepts three non-overlapping buffers using the System V x86-64 calling convention. Linux dispatch also provides hardware-selected paths. Hadeed links the scalar assembly object directly and calls it after its allocation tests.

The hosted oracle applies the engine's FP32 matrix-vector operation to each input column. Signed products and all intermediate sums are bounded by 1,048,576, within the exactly representable integer range of FP32. The guest uses a distinct structured matrix and a closed-form result for every output. The engine, C library and Linux syscall dispatch stay on the hosted side.

## Numeric and lifetime changes

CPU RMS normalization keeps the mean square, reciprocal root, and intermediate gradient products in double precision. The result for a two-element input of opposite-sign maximum finite FP32 values is +1 and −1 with unit weights. The interpreter grows its binding array on demand. It keeps a slot index across evaluation of an assignment's right-hand side because a nested call can reallocate the array.

## Boot and physical memory

The BIOS loader accepts 20- and 24-byte E820 records and stops if the map exceeds 128 entries. Kernel entry clears the zero-initialized and reserved sections before calling C. The guest reads back the active interrupt table, checks all 256 gate addresses and attributes, enables IRQ0, and waits for three ticks.

The buddy allocator tracks free blocks and live allocations by order. It accepts complete usable pages above 2 MiB and excludes any page touched by an active reserved range. Heap arenas use 16 adjacent 4 KiB pages. A mapping request checks the physical range, available table space and all existing leaves before replacing any absent leaf. A conflicting span leaves existing mappings intact.

## Assembly training

The attention model updates its parameters after each training example. Epoch `e` starts at zero and uses a learning rate of `0.025 × 32 / (32 + e)`. The first epoch uses 0.025 and epoch 32 uses 0.0125.

## Verification boundaries

QEMU boots the raw disk image with a legacy BIOS. The guest writes its long-mode, memory and tensor results to debug port 0xe9. The optional debug-exit device maps guest success to process exit 33. The verifier checks this status and a fresh transcript in a unique temporary directory. A timeout, an absent emulator, an unexpected transcript, and a triple fault fail the check.

The scalar language compiler emits Linux ELF programs. Hadeed's image provides the boot and memory tests above. The host tools and the trained-model runtime do not execute as guest applications. Inference quality requires a named model and a separate evaluation corpus.
