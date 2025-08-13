# Hello World Typescript

Workerd has several experimental features to support typescript.

Run this example using:

```bash
just watch run -- serve $(pwd)/samples/helloworld-ts/config.capnp
```

## Type stripping

This feature is enabled by `compatibilityFlags = ["typescript_strip_types"]`.

In this mode workerd strips Typescript types from loaded files (without type-checking) and fails
on any unsupported typescript construct (like enum).

## Transpilation

This feature is enabled by `compatibilityFlags = ["typescript_transpile"]`.

In this mode workerd transpiles Typescript files to JavaScript using swc transpiler.
