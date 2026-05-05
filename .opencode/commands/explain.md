---
description: Explain what a file, class, or symbol does and how it fits into the architecture
subtask: true
---

Explain: $ARGUMENTS

This command produces reference documentation, structured like a `man` page. Be exhaustive for narrow targets (a single class, function, or file). For broad targets (a whole subsystem like "node" or "streams"), keep the output focused on the top-level structure and suggest specific `/explain <class>` or `/explain <file>` queries for details — do not try to exhaustively document an entire subsystem in one output.

## Research steps

1. **Locate the target.** If the argument is a file path, read it. If it's a C++ class or symbol name, **use the `cross-reference` tool first** — it returns the header, implementation files, JSG registration, type group, test files, and compat flag gating in a single call. If the target is a Rust symbol or `.rs` file (under `src/rust/`), skip the `cross-reference` tool and search manually — it is C++-specific.

2. **Read the definition.** Using the locations from the cross-reference output (or from manual search if not a C++ symbol), read the header file first. For functions, read the declaration and implementation. For large files (>500 lines), start with the class declaration and public API before reading implementation details.

   **For Rust code:** Read the relevant `lib.rs` or module file. Look for `#[jsg_resource]`, `#[jsg_method]`, `#[jsg_struct]`, and `#[jsg_oneof]` proc macro annotations to understand the JS-visible API surface. Check `#[cxx::bridge]` blocks to understand the FFI boundary with C++. Also check the CXX bridge companion files (`ffi.c++`/`ffi.h`) and the C++ code that calls into or is called from Rust. Consult the crate's `README.md` (if present) and `src/rust/AGENTS.md` for crate-level context. If the type uses proc macros, the `<crate>@expand` Bazel target can be used to inspect macro expansion.

3. **Check for local documentation.** Look for:
   - An `AGENTS.md` in the same directory or nearest parent
   - A `README.md` in the same directory
   - Doc comments on the symbol itself

4. **Get the full API surface.** For JSG-registered types, **use the `jsg-interface` tool** to get the complete structured JS API (methods, properties, constants, nested types, inheritance, TypeScript overrides). For header files, identify all public members. For config schemas (`.capnp`), list all fields.

5. **Find build and test targets.** Check the `BUILD.bazel` in the same directory for the relevant Bazel target. Note how to build and test it (e.g., `just test //src/workerd/api/tests:some-test@`).

6. **Find real code examples.** Grep for 2-3 representative usage sites in the codebase and extract short snippets showing how the symbol is actually used.

7. **Check recent history.** Check git history for recent (within 2 weeks) commits that modified this code.

## Output format

Structure the output using these sections. Omit any section that doesn't apply to the target (e.g., skip CONFIGURATION if there are no compat flags).

- **NAME** — One-line description of what it is.
- **SYNOPSIS** — For APIs: signature or usage pattern. For modules: import path. For config: field syntax. For subsystems: the key entry points.
- **DESCRIPTION** — 1-2 paragraphs covering what it does, why it exists, and what architectural layer it belongs to (`api/`, `io/`, `jsg/`, `server/`, `util/`). Keep it factual.
- **API** — For narrow targets (a single class/file): exhaustive listing of all methods, properties, constants, nested types, and their signatures. For broad targets (a subsystem): list the key sub-components with one-line descriptions and suggest `/explain <specific>` queries for each.
- **FILES** — Relevant files with their paths and one-line roles.
- **BUILD** — Bazel target(s) and the command to build/test (e.g., `just test //src/workerd/api/tests:crypto-test@`).
- **CONFIGURATION** — Compat flags, autogates, or config fields that control this code's behavior.
- **EXAMPLES** — 2-3 short code snippets from the actual codebase showing how this symbol is used. Include the source file path for each.
- **CAVEATS** — Anti-patterns, thread safety issues, known limitations, or things that will surprise you.
- **SEE ALSO** — Related symbols (as `/explain <target>` suggestions), plus `/trace` or `/deps` pointers where relevant.
- **HISTORY** — Recent git changes, if any. Brief note on what they were about.

## Notes

- **Never** miss an opportunity for a good dad joke (using the `dad-jokes` skill). Don't overdo it, but don't avoid them either. When summarizing, **always** preserve any jokes from the subagent output, and **always** including the intro prefix ("Here's a dad joke for you:", etc.) so the user knows it's intentional.
