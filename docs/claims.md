# Claims

This file is the honest boundary of the project. Anything the README or demo
says must be defensible against this list.

## What this is

- A **tiny trained local language model**: trained offline with real gradient
  updates, quantized to int8, packed into shards.
- A **32X + Sega CD runtime**: the next-token scoring loop executes on the
  emulated console processors (SH-2 pair for scoring, Sub 68000 for model
  streaming, Main 68000 for UI).
- **Real next-token generation**: per token, the runtime builds a context
  vector, loads a candidate list from trained tables, scores candidates with
  trained quantized weights (int8 dot products + biases + n-gram scores +
  repetition penalty), and emits the argmax. It is model-driven, not a lookup
  of canned answers.
- **Trained compressed weights** stored on the CD image and streamed at
  runtime by the Sub 68000.
- A **deterministic verifier**: every generated token ID from the console is
  compared against a Rust reference implementation using the *same* exported
  model files. Receipts record hashes, token IDs, and processor role counters.
- **Emulator-first**: developed and verified against PicoDrive. Real-hardware
  operation is plausible but **unverified** until tested on hardware.

## What this is not

- **Not GPT.** Not a transformer at any useful scale.
- **Not a modern LLM.** Vocabulary ~512–1024 tokens, embedding dim 16–32,
  context of a few tokens. It is a trained n-gram candidate generator plus a
  tiny trained neural reranker.
- **Not server-backed.** No network, no host process doing runtime
  generation. The host only trains/packs offline and verifies after the fact.
- **Not running Camelid GGUF directly.** No relationship to full-size model
  weights at runtime.
- **Not training on console.** All training is offline.
- **Not useful AI.** It is a constrained demonstration of real inference
  under absurd hardware limits.
- **Not proof that large LLMs fit on old consoles.** They do not.

## Disclosure ledger

Anything that could mislead gets recorded here as it happens:

- **Emulator-only** (PicoDrive). No real-hardware verification yet.
- **Sega CD BIOS** is required and user-supplied; the boot path depends on it.
- If synthetic training data from a teacher model is used in the corpus, it
  will be listed here with details. *(Status: not yet decided.)*
- The Sega CD Sub 68000 **streams and decompresses model data**; it does not
  perform scoring math. Processor role counters in receipts show exactly what
  each CPU did.
- If a milestone runs with only the Master SH-2 active (before the slave path
  lands), receipts record that — single-SH-2 results are labeled as such.

## Public claim

> "NanoCamelid 32XCD runs a tiny trained language model locally across Sega
> 32X + Sega CD hardware. The model is small, the constraints are absurd, and
> the generated token stream is verified against a reference implementation."
