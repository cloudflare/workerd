# Workers Types Generator

This directory contains scripts for automatically generating TypeScript types
from [JSG RTTI](../src/workerd/jsg/rtti.h).

## Generating Types

```shell
# Generates types to `../bazel-bin/types/api.d.ts`
$ bazel build //types:types
```

## Developing Generator Scripts

```shell
# Generates JSG RTTI Cap’n Proto JavaScript/TypeScript files
$ bazel build //src/workerd/jsg:rtti_capnp_js
# Install dependencies (note pnpm is required by https://github.com/aspect-build/rules_js)
$ pnpm install
```

## Structure

- `src/generator`: generating TypeScript AST nodes from JSG RTTI
- `src/transforms`: post-processing TypeScript AST transforms
- `src/index.ts`: main entrypoint
- `src/{print,program}.ts`: helpers for printing nodes and creating programs
- `workerd`: symlink required to resolve JSG RTTI Cap’n Proto files during development
