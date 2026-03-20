# Advanced `.wd-test` Config Patterns

Patterns for Durable Objects, multi-service tests, network access, external services,
and TypeScript tests.

---

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
