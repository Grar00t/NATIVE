# C11 model engine

The library implements CPU FP32 transformer training, checkpoint persistence, tokenization and autoregressive decoding. The repository build places the command-line tools in `build/engine`.

Use `niyah prepare` to tokenize a corpus and write a shard, `niyah-train new` to train a checkpoint, and `niyah run` to decode. Each command reports its argument contract through the CLI. [Command reference](docs/CLI.md) · [Checkpoint publication](docs/PERSISTENCE_PROTOCOL.md)

The root Makefile selects the static CPU build. Optional CMake flags enable CUDA or OpenMP. Weights and training records are supplied by the caller.

## Read-only weight files

Include `niyah/model_mmap.h` and call `niyah_model_mmap_load(path, &model)` with a zero-initialized `NiyahModel`. The loader maps little-endian raw FP32 model files with a 64-byte v5 header into memory for inference. Release the mapping with `niyah_model_destroy`. Training checkpoints use a different file format.

The optional external regression uses the 5,997,120-byte reference model. Set `NIYAH_MMAP_MODEL_PATH` and `NIYAH_MMAP_MODEL_SHA256` together to verify its identity and run a forward pass. Existing applications using the previous loader symbol and header remain compatible.
