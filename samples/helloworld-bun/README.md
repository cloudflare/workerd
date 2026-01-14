# Bun Hello World

Demonstrates Bun APIs in workerd using the bundled compatibility layer.

## Build

```bash
# Build the Bun compatibility bundle
cd ../../src/bun && bun run build.ts

# Build the worker (TypeScript → JavaScript)
cd ../../samples/helloworld-bun
bun -e "
import { build } from 'esbuild'
await build({
  entryPoints: ['worker.ts'],
  outfile: 'worker.js',
  format: 'esm',
  bundle: true,
  external: ['./bun-bundle.js'],
})
"
```

## Run

Create a local config for your workerd version:

```bash
cat > config-local.capnp << 'EOF'
using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (
  services = [ (name = "main", worker = .w) ],
  sockets = [ ( name = "http", address = "*:9124", http = (), service = "main" ) ]
);
const w :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js"),
    (name = "./bun-bundle.js", esModule = embed "../../dist/bun/bun-bundle.js")
  ],
  compatibilityDate = "2024-09-02",
  compatibilityFlags = ["nodejs_compat_v2"]
);
EOF

workerd serve config-local.capnp
```

## Test

```bash
curl http://localhost:9124/
curl "http://localhost:9124/hash?data=hello"
curl http://localhost:9124/deep-equals
curl http://localhost:9124/escape-html
curl http://localhost:9124/stream
curl http://localhost:9124/sleep?ms=100
curl http://localhost:9124/health
```

## Endpoints

| Endpoint | Description |
|----------|-------------|
| `/` | Version and uptime |
| `/hash?data=X` | Hash function |
| `/deep-equals` | Object comparison |
| `/escape-html` | HTML escaping |
| `/nanoseconds` | High-res timestamp |
| `/inspect` | Object formatting |
| `/string-width` | Unicode width |
| `/array-buffer-sink` | Buffer sink |
| `/stream` | Stream utilities |
| `/sleep?ms=X` | Async sleep |
| `/health` | Health check |
