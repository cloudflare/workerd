---
name: wd-test-format
description: Detailed guide for authoring .wd-test files in workerd, with examples of bindings, Durable Objects, multi-service configs, TypeScript tests, and network access.
---

## `.wd-test` File Format

`.wd-test` files are Cap'n Proto configs that define test workers for workerd's test framework. They use the schema defined in `src/workerd/server/workerd.capnp`.

---

### MANDATORY: Load Reference File When Relevant

This skill is split across multiple files for context efficiency. The core patterns below cover
standard single-service tests. Advanced configuration patterns live in a reference file.

**You MUST read the reference file before writing or reviewing test configs that involve its
subject matter. Do not guess at advanced config syntax — the reference file contains the exact
patterns and fields required. Skipping it WILL lead to incorrect configs that fail at runtime.**

| File                            | MUST load when...                                            |
| ------------------------------- | ------------------------------------------------------------ |
| `reference/advanced-configs.md` | Test involves Durable Objects, multiple services             |
|                                 | communicating via service bindings, outbound network access, |
|                                 | external services/sockets, or TypeScript source files        |

When in doubt about whether the reference file is relevant, load it — the cost of reading is
far less than the cost of a broken test config.

---

### Basic Structure

```capnp
using Workerd = import "/workerd/workerd.capnp";

const unitTests :Workerd.Config = (
  services = [(
    name = "my-test",
    worker = (
      modules = [(name = "worker", esModule = embed "my-test.js")],
      compatibilityFlags = ["nodejs_compat_v2"],
    ),
  )],
);
```

Key rules:

- The const name (e.g., `unitTests`) must match what the test runner expects
- `modules` uses `embed` to inline file contents at build time
- The first module should be named `"worker"` — this is the entry point
- `compatibilityFlags` control which APIs are available. Use the `compat-date-at` tool to look up available flags and their enable dates.
- `compatibilityDate` should not be used in wd-test; use specific flags instead

### Module Types

```capnp
modules = [
  (name = "worker", esModule = embed "my-test.js"),           # ES module (most common)
  (name = "helper", esModule = embed "helper.js"),            # Additional ES module
  (name = "data.json", json = embed "test-data.json"),        # JSON module
  (name = "data.wasm", wasm = embed "module.wasm"),           # WebAssembly module
  (name = "legacy", commonJsModule = embed "legacy.js"),      # CommonJS module
],
```

### Bindings

Bindings make services, data, and namespaces available to the worker via `env`:

```capnp
bindings = [
  # Text binding — env.MY_TEXT is a string
  (name = "MY_TEXT", text = "hello world"),

  # Text from file
  (name = "CERT", text = embed "fixtures/cert.pem"),

  # Data binding — env.MY_DATA is an ArrayBuffer
  (name = "MY_DATA", data = "base64encodeddata"),

  # JSON binding — env.CONFIG is a parsed object
  (name = "CONFIG", json = "{ \"key\": \"value\" }"),

  # Service binding — env.OTHER_SERVICE is a fetch-able service
  (name = "OTHER_SERVICE", service = "other-service-name"),

  # Service binding with entrypoint
  (name = "MY_RPC", service = (name = "my-service", entrypoint = "MyClass")),

  # KV namespace — env.KV is a KV namespace
  (name = "KV", kvNamespace = "kv-namespace-id"),

  # Durable Object namespace — env.MY_DO is a DO namespace
  (name = "MY_DO", durableObjectNamespace = "MyDurableObject"),
],
```

### Test JavaScript Structure

Test files export named objects with a `test()` method:

```javascript
// Each export becomes a separate test case
export const basicTest = {
  test() {
    // Synchronous test
    assert.strictEqual(1 + 1, 2);
  },
};

export const asyncTest = {
  async test(ctrl, env) {
    // ctrl is the test controller
    // env contains bindings from the .wd-test config
    const resp = await env.OTHER_SERVICE.fetch('http://example.com/');
    assert.strictEqual(resp.status, 200);
  },
};
```

### BUILD.bazel Integration

```python
wd_test(
    src = "my-test.wd-test",
    args = ["--experimental"],      # Required for experimental features
    data = ["my-test.js"],          # Test JS/TS files
)
```

Additional `data` entries for fixture files:

```python
wd_test(
    src = "crypto-test.wd-test",
    args = ["--experimental"],
    data = [
        "crypto-test.js",
        "fixtures/cert.pem",
        "fixtures/key.pem",
    ],
)
```

### Test Variants

Every `wd_test()` automatically generates three variants:

| Target suffix       | Compat date | Description                            |
| ------------------- | ----------- | -------------------------------------- |
| `@`                 | 2000-01-01  | Default, tests with oldest compat date |
| `@all-compat-flags` | 2999-12-31  | Tests with all flags enabled           |
| `@all-autogates`    | 2000-01-01  | Tests with all autogates enabled       |

Run specific variants:

```bash
just stream-test //src/workerd/api/tests:my-test@
just stream-test //src/workerd/api/tests:my-test@all-compat-flags
```

### Scaffolding

Use `just new-test` to scaffold a new test:

```bash
just new-test //src/workerd/api/tests:my-test
```

This creates the `.wd-test` file, `.js` test file, and appends the `wd_test()` rule to `BUILD.bazel`.
