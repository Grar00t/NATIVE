# Generation repetition control

Set `--repetition-penalty` to a finite value of at least 1 and `--repetition-window` to a positive token count. A penalty of 1 uses the ordinary sampling path. A larger value adjusts scores of tokens emitted within the selected history window. The generation buffer holds that history.

The enabled CPU path needs the decode workspace and two additional vocabulary-sized float arrays for raw logits and sampling scratch. Call `niyah_generation_workspace_floats_with_repetition` for the required size. Callers own the buffers and must supply disjoint storage. On enabled-path failure the routine resets the cache and clears the result. Discard output tokens from a failed call.

The CPU tests cover disabled-path equivalence, invalid configurations, scratch capacity, greedy and seeded sampling, and repeated-token behavior. The optional CUDA build has separate integration tests. The [public API](../include/niyah/generate.h) specifies buffer ownership and the stop reports.
