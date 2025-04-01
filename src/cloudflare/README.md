# Cloudflare specific modules

## tests

This codebase includes many unit tests. To run them, do:

```
bazel test  --test_output=all //src/cloudflare/...
```

Running just a specific module tests:

```
bazel test  --test_output=all //src/cloudflare/internal/test/aig/...
```

Running just eslint:

```
bazel test  --test_output=all //src/cloudflare:cloudflare@eslint
```

## Code formating

You need to format your code before opening a pull request, otherwise the CI will fail:

```
prettier src/cloudflare -w
prettier types/defines -w
```
