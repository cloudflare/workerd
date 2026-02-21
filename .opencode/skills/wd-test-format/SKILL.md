---
name: wd-test-format
description: Detailed guide for authoring .wd-test files in workerd, with examples of bindings, Durable Objects, multi-service configs, TypeScript tests, and network access.
---

## `.wd-test` File Format

`.wd-test` files are Cap'n Proto configs that define test workers for workerd's test framework. They use the schema defined in `src/workerd/server/workerd.capnp`.

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
- `compatibilityFlags` control which APIs are available
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

### Durable Objects

To test Durable Objects, define the namespace and storage:

```capnp
const unitTests :Workerd.Config = (
  services = [
    ( name = "do-test",
      worker = (
        modules = [(name = "worker", esModule = embed "do-test.js")],
        compatibilityDate = "2024-01-01",

        durableObjectNamespaces = [
          (className = "MyDurableObject", uniqueKey = "210bd0cbd803ef7883a1ee9d86cce06e"),
        ],

        durableObjectStorage = (localDisk = "TEST_TMPDIR"),

        bindings = [
          (name = "MY_DO", durableObjectNamespace = "MyDurableObject"),
        ],
      ),
    ),
    # Disk service for DO storage
    (name = "TEST_TMPDIR", disk = (writable = true)),
  ],
);
```

The `uniqueKey` is a hex string that uniquely identifies the namespace. Use any 32-char hex string for tests.

For in-memory storage (no persistence between requests), use:

```capnp
durableObjectStorage = (inMemory = void),
```

### Multi-Service Configs

Tests can define multiple services that communicate via service bindings:

```capnp
const unitTests :Workerd.Config = (
  services = [
    ( name = "main-test",
      worker = (
        modules = [(name = "worker", esModule = embed "main-test.js")],
        compatibilityDate = "2024-01-01",
        bindings = [
          (name = "BACKEND", service = "backend"),
        ],
      ),
    ),
    ( name = "backend",
      worker = (
        modules = [(name = "worker", esModule = embed "backend.js")],
        compatibilityDate = "2024-01-01",
      ),
    ),
  ],
);
```

For large configs, factor out worker definitions as named constants:

```capnp
const unitTests :Workerd.Config = (
  services = [
    (name = "main", worker = .mainWorker),
    (name = "helper", worker = .helperWorker),
  ],
);

const mainWorker :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "main.js")],
  compatibilityDate = "2024-01-01",
  bindings = [(name = "HELPER", service = "helper")],
);

const helperWorker :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "helper.js")],
  compatibilityDate = "2024-01-01",
);
```

### Network Access

For tests that need outbound network access:

```capnp
( name = "internet",
  network = (
    allow = ["private"],
    tlsOptions = (
      trustedCertificates = [
        embed "test-cert.pem",
      ],
    ),
  )
),
```

`allow` can be `["private"]` (loopback/LAN) or `["public"]` (internet). Most tests use `"private"`.

### External Services

For testing RPC over sockets or external service communication:

```capnp
( name = "my-external",
  external = (
    address = "loopback:my-external",
    http = (capnpConnectHost = "cappy")
  )
),
```

### TypeScript Tests

TypeScript test files use a `.ts-wd-test` extension for the config, but the embedded module is the compiled `.js` output:

**Config file** (`my-test.ts-wd-test`):

```capnp
using Workerd = import "/workerd/workerd.capnp";

const unitTests :Workerd.Config = (
  services = [(
    name = "my-test",
    worker = (
      modules = [(name = "worker", esModule = embed "my-test.js")],
      compatibilityDate = "2024-01-01",
    ),
  )],
);
```

**BUILD.bazel** references the `.ts` source:

```python
wd_test(
    src = "my-test.ts-wd-test",
    args = ["--experimental"],
    data = ["my-test.ts"],
)
```

The build system compiles the `.ts` to `.js` automatically.

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
