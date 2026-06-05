# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

The parent repo's `../CLAUDE.md` covers the agentic C89 port goal, the working principles, and where planning artifacts (`PLAN.md`, `PHASE*.md`, `MEMORY.md`) must live (top-level `agentic-c89/`, never inside `theft/`). Read it first. This file documents only what is specific to theft's own architecture.

## Build / test

```
make                         # build/libtheft.a + build/test_theft (GNU make; gmake on BSD)
make test                    # run all tests; expect intentionally falsifiable properties to "fail"
./build/test_theft -t NAME   # single greatest test
./build/test_theft -s SUITE  # one suite
make clean
```

`-lm` is linked for built-in float generators. No external runtime deps; `vendor/greatest.h` is bundled.

## Architecture

theft is a property-based tester. A *run* generates random inputs from user-supplied *type info*, evaluates a *property* on them, and on failure *shrinks* the input toward a minimal counter-example. The code splits along those phases — each has a public-facing file plus an `_internal.h` header used by sibling modules:

- `theft_run.{c,h}` — top-level loop: orchestrates trials, owns the `theft` run state, drives the RNG seed schedule. The struct definitions for run/trial state are in `src/theft_types_internal.h`.
- `theft_trial.{c,h}` — one trial: generate args, hash them, check the bloom filter for repeats, invoke the property, classify the result (PASS/FAIL/SKIP/DUPLICATE/ERROR), report via hooks.
- `theft_call.{c,h}` — actually invokes the user's property function with the right arity (1..N args). C89 port note: arity dispatch here is one of the few places that may want per-arity helpers rather than variadic macros.
- `theft_shrink.{c,h}` — manual shrinking: walks each argument's `shrink` callback to find smaller failing inputs.
- `theft_autoshrink.{c,h}` — automatic shrinking built on top of a recorded *bit pool* (the random bits consumed during generation). Replays generation against a smaller pool to derive simpler inputs, without the user writing a `shrink` callback. `theft_autoshrink_internal.h` exposes the pool layout to tests.
- `theft_random.{c,h}` — the bit-stream interface user generators consume (`theft_random_bits`, etc.). Backed by `theft_rng` in autoshrink-off mode, or by the bit pool in autoshrink-on mode.
- `theft_rng.{c,h}` — Mersenne Twister. **Most C89-sensitive file**: 64-bit state. Port must replace `uint64_t` with a typedef or a 32-bit pair.
- `theft_hash.c` — FNV-1a, used to detect duplicate trials.
- `theft_bloom.{c,h}` — bloom filter of seen-input hashes; lets the runner skip duplicates cheaply.
- `theft_aux.c` / `theft_aux_builtin.c` — built-in type generators (ints, floats, chars, arrays). New built-ins go in `theft_aux_builtin.c`.

Public surface is just `inc/theft.h` and `inc/theft_types.h`. Keep these source-compatible during the C89 port — downstream users include them directly. All internal headers stay in `src/`.

`pc/` holds a `pkg-config` template; `scripts/` has install/uninstall helpers; `doc/` is reference markdown (read `doc/forking.md` before touching fork-mode code in `theft_call.c` / `theft_trial.c`).

## Tests

Tests live in `test/` and are linked into one binary, `test_theft`. Each `test_*.c` is a greatest suite; `test_theft.c` is the entry point that registers them. The autoshrink suite is split across several files because it exercises the bit-pool machinery hard. Failing-property output is *expected* in `make test` — those are demonstrations, not regressions.
