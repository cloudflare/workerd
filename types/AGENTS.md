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

## ADDING OR UPDATING TYPES

### Where type changes belong

Types in this project come from three layers. Changes must be made in the correct layer:

| Layer                    | Location                                           | When to use                                                                                                |
| ------------------------ | -------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| **C++ RTTI**             | `src/workerd/api/*.h` (JSG macros)                 | Types derived from C++ classes — the generator extracts these automatically                                |
| **`JSG_TS_OVERRIDE`**    | Same `.h` files, inside `JSG_RESOURCE_TYPE` blocks | When the auto-generated TS signature needs adjustment (e.g., conditional properties, union narrowing)      |
| **Hand-written defines** | `types/defines/*.d.ts`                             | Types for APIs not expressible via C++ RTTI (Cloudflare product bindings like AI, D1, R2, Vectorize, etc.) |

Do **not** edit files in `generated-snapshot/` directly — they are overwritten by `just generate-types`. If the generated output looks wrong, fix the source layer (C++ RTTI, `JSG_TS_OVERRIDE`, or `defines/`).

### Naming conventions

Types in `defines/*.d.ts` become **top-level ambient declarations** visible to every Worker project. Generic names will collide or confuse.

- **Prefix types with the product or feature name.** For example: `D1Meta`, `AiSearchConfig`, `VectorizeVector` — not `Meta`, `Config`, or `Vector`.
- **Follow the existing convention** for the relevant product. Check the existing `defines/` file for that product (e.g., `cf.d.ts` uses `IncomingRequestCfProperties*`, `d1.d.ts` uses `D1*`, `ai-search.d.ts` uses `AiSearch*`).
- **When in doubt, be more specific.** A name like `TracePreviewInfo` is better than `Preview`. The cost of a verbose name is low; the cost of a naming collision in every user's project is high.

### Interface design

- **Never use bare `object` as a type.** `type Foo = object` prevents all property access without casting, providing no value to users. Use a proper interface with known fields instead.
- **Use interfaces with known fields + index signature** for types where the shape is partially known but extensible. Follow the `IncomingRequestCfPropertiesBase` pattern:
  ```typescript
  interface CloudflareAccessIdentity extends Record<string, unknown> {
    email?: string;
    name?: string;
    // ... other known fields
  }
  ```
- **Index signatures (`[key: string]: unknown`) need justification.** Only add them when forward-compatibility is genuinely required (e.g., the API frequently adds new fields across releases). Document the reason in a JSDoc comment. Do not add index signatures by default.
- **Add JSDoc comments** to interfaces and their fields, especially for types used in `defines/`. These comments appear in users' IDEs.

### Snapshot regeneration

After any change that affects types (C++ API changes, `JSG_TS_OVERRIDE` edits, `defines/` modifications), regenerate the snapshot:

```shell
just generate-types
```

CI will fail if `generated-snapshot/` does not match the generated output. Always commit the updated snapshot alongside your source changes.

### Type tests

Type-level tests live in `test/types/` (e.g., `test/types/rpc.ts`). These are compile-time checks — they verify that type expressions are accepted or rejected by the TypeScript compiler.

- When adding new types or changing existing ones, add or update type tests to cover the new shapes.
- Review type test changes carefully — a test that compiles successfully may still be asserting the wrong thing.

### PR hygiene for type changes

- **Do not include unrelated formatting changes** in PRs that modify types. If formatting needs fixing, do it in a separate commit or PR.
- **Type changes in feature PRs need review** from someone familiar with the types system. The Wrangler team typically reviews type changes that affect `@cloudflare/workers-types`.
