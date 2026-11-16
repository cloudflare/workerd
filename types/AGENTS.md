# types/

## OVERVIEW

Generates `@cloudflare/workers-types` `.d.ts` files from C++ and Rust JSG RTTI (via `jsg/rtti.capnp`), the declarations TypeScript emits for the per-isolate bootstrap (`src/per_isolate`), and hand-written `defines/*.d.ts`. Rust descriptions come from `TypeInfo` and typed module registration in `src/rust/jsg/`; internal module registration does not make a type an ambient root. A workerd-hosted Worker extracts RTTI at runtime per compat-date; TypeScript transforms post-process into ambient and importable outputs. CI validates `generated-snapshot/` matches.

## PIPELINE

1. **RTTI extraction**: `src/worker/` runs inside workerd, imports binary RTTI via `workerd:rtti` + param names from `param-extractor.rs`
2. **Generation**: `src/generator/` converts Cap'n Proto structures to TS AST nodes (walks types, members, methods recursively via `collectIncluded`)
3. **Transforms** (ordered, in `src/index.ts`): iterators -> overrides/defines -> global scope extraction -> class-to-interface -> comments -> onmessage declaration; then second pass: import-resolve -> ambient. Importable variant runs separately.
4. **Per-isolate declarations**: `src/per-isolate.ts` collects, when the worker is bundled, what `src/per_isolate/public-types.d.ts` publishes (see "Per-isolate declarations" below). They get the comments transform only, and follow the generated declarations.
5. **Defines concatenation**: `defines/*.d.ts` appended after transforms (AI, D1, R2, RPC, Vectorize, etc.)
6. **Build**: `scripts/build-types.ts` generates the latest compatibility date (2999-12-31) and experimental entrypoints, which `generated-snapshot/` holds, and type-checks them against TS libs. The worker in `src/worker/` generates types for any compatibility date and flags (`/<compat-date>(+<flag>)*`), but the snapshot holds only these two

### Explicit RTTI metadata

- **`Structure.tsName` / `StructureType.tsName`** supply the emitted declaration
  and reference name. When empty, the generator derives the name from
  `fullyQualifiedName`, which remains the identity used to resolve references.
- **`Method.argNames` / `Constructor.argNames`** supply parameter names aligned
  with `args`, including slots for internal arguments that are omitted from the
  declaration. A missing or empty entry falls back to the C++ parameter-name
  table, then `paramN`. Reserved binding names are escaped.
- **`MaybeType.allowsNull` / `MaybeType.allowsUndefined`** add the corresponding
  alternatives to the wrapped type. When both are false, legacy decoding uses
  `name`: `kj::Maybe` means `null`, and other names mean `undefined`.

Explicit-name conflicts are checked among emitted types. An inactive type with
the same emitted name cannot supply overrides or defines for the selected type.

## KEY DIRECTORIES

| Directory             | Purpose                                                                          |
| --------------------- | -------------------------------------------------------------------------------- |
| `src/generator/`      | RTTI-to-TS-AST: `structure.ts` (classes/interfaces), `type.ts` (type mapping)    |
| `src/per-isolate.ts`  | Collects the per-isolate bootstrap's published declarations                      |
| `src/transforms/`     | 11 post-processing TS transformer factories                                      |
| `src/worker/`         | Workerd-hosted entry; imports RTTI binary + virtual modules                      |
| `defines/`            | Hand-written `.d.ts` for APIs not expressible via C++ RTTI (~25 files)           |
| `generated-snapshot/` | Checked-in latest date (top level) + `experimental/` output; CI diff-checked     |
| `scripts/`            | `build-types.ts` (full generation), `build-worker.ts` (worker bundle)            |
| `test/`               | Vitest specs for generator, transforms, print; type-check tests in `test/types/` |

## ADDING OR UPDATING TYPES

### Where type changes belong

Types in this project come from four layers. Changes must be made in the correct layer:

| Layer                    | Location                                           | When to use                                                                                                |
| ------------------------ | -------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| **C++ RTTI**             | `src/workerd/api/*.h` (JSG macros)                 | Types derived from C++ classes — the generator extracts these automatically                                |
| **`JSG_TS_OVERRIDE`**    | Same `.h` files, inside `JSG_RESOURCE_TYPE` blocks | When the auto-generated TS signature needs adjustment (e.g., conditional properties, union narrowing)      |
| **Per-isolate bootstrap** | `src/per_isolate/**/*.ts`, listed in `src/per_isolate/public-types.d.ts` | APIs the per-isolate bootstrap implements in TypeScript and publishes in its manifest (`DOMException`, `URL`, `URLSearchParams`, `URLPattern`); their C++ counterparts declare `JSG_TS_OVERRIDE(type X = never)`. See "Per-isolate declarations" below |
| **Hand-written defines** | `types/defines/*.d.ts`                             | Types for APIs not expressible via C++ RTTI (Cloudflare product bindings like AI, D1, R2, Vectorize, etc.), and for `structuredClone()` (`structured-clone.d.ts`), a global scope method the bootstrap implements, which the manifest cannot publish; its C++ counterpart is a `never` member in the global scope's override |

Do **not** edit files in `generated-snapshot/` directly — they are overwritten by `just generate-types`. If the generated output looks wrong, fix the source layer (C++ RTTI, `JSG_TS_OVERRIDE`, the per-isolate implementation, or `defines/`).

### Per-isolate declarations

A class the per-isolate bootstrap implements declares its public surface in
its own TypeScript, and `src/per_isolate/public-types.d.ts` publishes it with
`export type { Name } from './module'`, which needs the module to
`export type { Name }`. The generator copies each published declaration from
the declaration file TypeScript emits for the module (with `stripInternal`),
in the manifest's order, at every compatibility date. Because what TypeScript
emits is what users get:

- **Overload signatures are the public signatures.** A method or constructor
  that converts its arguments as WebIDL does takes `unknown`; declare the
  public signature as an overload above it. TypeScript emits overloads and
  leaves the implementation signature out. An argument only internal callers
  pass gets an extra overload marked `/** @internal */`.
- **What the implementation installs itself is declared, not implemented.**
  A static installed with `ObjectDefineProperties` is a
  `declare static readonly NAME: type;` field. A prototype member, or a
  prototype parent, set up outside the class body goes in an interface
  merged with the class, which the generator folds into the class's
  declaration: its members join the class's, and its one `extends` becomes
  the class's, if the class has none.
- **Member form is API.** A getter and a property are not interchangeable:
  a subclass overriding one as the other fails to compile (TS2610, TS2611),
  so changing a member's form is a breaking type change.
- **A declaration the class cannot emit is written by hand.** When the
  published declaration takes a form TypeScript does not emit for a class,
  such as the standard library's interface with a `declare var` for its
  constructor, which `DOMException` has, it is a declaration file beside
  the implementation (`dom-exception.public.d.ts`), which the manifest
  publishes in place of the module's. The implementation checks that it
  agrees with the declaration, with `satisfies`, so that the two cannot
  drift.
- **A published declaration refers only to globals and published
  declarations.** The generator rejects a reference to another of the
  bootstrap's declarations, and a published name that RTTI or a define also
  declares.
- `#private` members, `@internal` members and comments other than JSDoc do
  not reach the output, from a hand-written declaration file either. JSDoc
  does, and the comments transform adds the standard MDN comments by name.

### Porting an API to the bootstrap

An API's types move with it, in the change that installs its TypeScript
class (see `src/per_isolate/AGENTS.md` for the bootstrap's conventions):

1. Declare the class's public surface as above, `export type` it from its
   module, and list it in `src/per_isolate/public-types.d.ts` with the
   dictionaries and other declarations its signatures name. A name's place in
   the manifest is its place in the output.
2. Remove the declarations RTTI generates for the C++ class: add
   `JSG_TS_OVERRIDE(type Name = never);` to its `JSG_RESOURCE_TYPE` block, and
   `JSG_STRUCT_TS_OVERRIDE(type Name = never);` after each `JSG_STRUCT` the
   manifest now publishes, each with a comment naming the file that declares
   it (see `src/workerd/api/urlpattern-standard.h`). The generator rejects a
   name that both RTTI and the manifest declare.
3. Delete the API's file in `defines/`, if it has one. The generator rejects
   a name that both a define and the manifest declare.
4. Run `just generate-types`. If the published types are unchanged, the
   snapshot's declarations only move ahead of the defines, and
   `git diff --histogram types/generated-snapshot` shows a move. Any other
   changed line changes the published types: make each such change
   deliberately, and say so in the commit message.
5. Keep, or add, a type test in `test/types/` (see "Type tests" below) that
   includes `self.Name`.

The declaration must also hold across:

- **Autogates.** The types do not vary by autogate, and a port usually sits
  behind one, with the C++ class as the fallback. The declaration must then
  be true of both classes. What only the TypeScript class provides stays
  unpublished until the autogate is removed, as `QuotaExceededError` does
  (`src/per_isolate/dom-exception.ts`).
- **Compatibility dates.** RTTI generates the C++ class's declaration at each
  Worker's compatibility date, from the members its `JSG_RESOURCE_TYPE`
  block registers under that date's flags, and the global scope
  (`src/workerd/api/global-scope.h`) may pick a different C++ class by
  flag, as it does for `URL` and `URLPattern`. A per-isolate declaration is
  the same at every date, and the snapshot shows only the latest date and
  experimental. Where the C++ declaration varies, publish the latest date's
  shape, and say beside it what older dates lack or ignore, as
  `urlpattern.ts` does for `hasRegExpGroups` and `url.ts` for the `value`
  argument of `delete()` and `has()`. A member that only an experimental
  flag enables would appear at every date, so it stays unpublished.

`self.Name` is declared by `ServiceWorkerGlobalScope`'s `Name: typeof Name`,
which RTTI generates from the C++ class's nested type in the global scope.
An API without one, because it has no C++ class or the port deletes it, has
no `self.Name` until the port declares it some other way; the type test's
`self.Name` check fails until it does.

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

After any change that affects types (C++ API changes, `JSG_TS_OVERRIDE` edits, signatures of published per-isolate classes, `defines/` modifications), regenerate the snapshot:

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
