# types/

## OVERVIEW

Generates `@cloudflare/workers-types` `.d.ts` files from C++ RTTI (via `jsg/rtti.capnp`) + hand-written `defines/*.d.ts`. A workerd-hosted Worker extracts RTTI at runtime per compat-date; TypeScript transforms post-process into ambient and importable outputs. CI validates `generated-snapshot/` matches.

## PIPELINE

1. **RTTI extraction**: `src/worker/` runs inside workerd, imports binary RTTI via `workerd:rtti` + param names from `param-extractor.rs`
2. **Generation**: `src/generator/` converts Cap'n Proto structures to TS AST nodes (walks types, members, methods recursively via `collectIncluded`)
3. **Transforms** (ordered, in `src/index.ts`): iterators -> overrides/defines -> global scope extraction -> class-to-interface -> comments -> onmessage declaration; then second pass: import-resolve -> ambient. Importable variant runs separately.
4. **Defines concatenation**: `defines/*.d.ts` appended after transforms (AI, D1, R2, RPC, Vectorize, etc.)
5. **Build**: `scripts/build-types.ts` iterates compat-date entrypoints (oldest through experimental), type-checks output against TS libs

## KEY DIRECTORIES

| Directory             | Purpose                                                                          |
| --------------------- | -------------------------------------------------------------------------------- |
| `src/generator/`      | RTTI-to-TS-AST: `structure.ts` (classes/interfaces), `type.ts` (type mapping)    |
| `src/transforms/`     | 11 post-processing TS transformer factories                                      |
| `src/worker/`         | Workerd-hosted entry; imports RTTI binary + virtual modules                      |
| `defines/`            | Hand-written `.d.ts` for APIs not expressible via C++ RTTI (~25 files)           |
| `generated-snapshot/` | Checked-in `latest/` + `experimental/` output; CI diff-checked                   |
| `scripts/`            | `build-types.ts` (full generation), `build-worker.ts` (worker bundle)            |
| `test/`               | Vitest specs for generator, transforms, print; type-check tests in `test/types/` |
