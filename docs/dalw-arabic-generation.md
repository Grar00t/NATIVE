# Arabic retrieval and generation on 26 November 2026

Dalw produced the ranked receipts. For each hit the evaluator read the recorded byte range from the local source file, matched its SHA-256 and checked the literal range with dalw-cli cite before adding any text to the prompt. The same four held-out questions ran with the resulting passages and with an empty context. The gold values were kept outside the prompts.

The generator was llama-completion from llama.cpp 0.2.0-dev, Debian build 10566, on x86-64 Linux. The Qwen2.5-0.5B-Instruct Q4_K_M GGUF had SHA-256 74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db. Each invocation loaded the model locally and used the ChatML prompt format, 1,024 context tokens, a 48-token output limit, four CPU threads, zero GPU layers, seed 17 and temperature zero.

## Four held-out questions

| Question | Expected response | Evidence rank | Without passages | With passages | Decode ms, without / with |
| --- | --- | --- | --- | --- | ---: |
| ما رمز خزانة سدرة؟ | 4173 | 1 | S | 4173 | 259.68 / 95.45 |
| ما لون بطاقة مرسى؟ | بنفسجي | 1 | Red | Purple | 539.10 / 194.95 |
| كم يبلغ وزن صندوق نوار بالكيلوغرام؟ | غير مذكور | 1, description omits weight | 1000000 | 120000 | 165.40 / 135.72 |
| ما رقم رف صندوق وهج؟ | تعارض | 1 and 2, shelves 12 and 19 | 1 | 12 | 36.37 / 75.59 |

Answer correctness was scored by meaning. With passages the model answered both supported questions correctly. It supplied a number for the missing weight and chose one of the conflicting shelves. All four answers without passages were incorrect. Dalw's digest and offset checks succeeded for all eight returned hit occurrences across the four searches. The timing column is the runtime's token evaluation measurement. Wall times including model startup ranged from 3.844 to 62.490 seconds under concurrent build load.

The first question retrieved document 0 at rank 1. Its source range began at byte 24 and contained 37 bytes with SHA-256 a7b544b2565492404f40a743944dbeda640ec6d31f6cef834ca4abb156b2a1db. Those bytes decoded to «رمز خزانة سدرة هو 4173.». The model returned 4173.

## Existing Arabic fixture

On the earlier 842-document shard, 921 queries produced 4,593 top-five receipts identical byte for byte to the baseline. Both batch-match modes and all 842 literal citations also matched. The synthetic ranking dump matched across 21,400 lines.

The existing question «من هو مخترع المحرك البخاري؟» retrieved document 7 at rank 1. The generator received the verified 1,635 bytes at offset 4,632. Their SHA-256 was 9edcb408aaf1542cc140158d326a5a4cfa14ed2130615a2f90ebacc48496c7bf. It returned «جيمس واط», the name in the passage. This check measures agreement with the cited passage.
