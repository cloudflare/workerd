# Bun Runtime Compatibility Layer for Workerd

Bun-compatible APIs for running Bun applications on workerd/Cloudflare Workers.

## Quick Start

```javascript
import Bun from './bun-bundle.js'

export default {
  async fetch(request) {
    const hash = Bun.hash('hello world')
    await Bun.write('/tmp/data.txt', 'Hello from Bun')
    const content = await Bun.file('/tmp/data.txt').text()
    
    return new Response(JSON.stringify({
      hash: hash.toString(16),
      content,
      version: Bun.version
    }))
  }
}
```

## API Status

| API | Status | Notes |
|-----|--------|-------|
| `Bun.hash` | ✅ | wyhash, crc32, adler32, cityhash, murmur |
| `Bun.password.hash/verify` | ✅ | PBKDF2 via WebCrypto |
| `Bun.file/write` | ✅ | node:fs (requires nodejs_compat) |
| `Bun.dns` | ✅ | DNS-over-HTTPS (Cloudflare/Google) |
| `Bun.sleep/sleepSync` | ✅ | setTimeout/busy-wait |
| `Bun.deepEquals` | ✅ | Deep object comparison |
| `Bun.escapeHTML` | ✅ | HTML entity escaping |
| `Bun.inspect` | ✅ | Object formatting |
| `Bun.readableStreamTo*` | ✅ | Stream utilities |
| `Bun.ArrayBufferSink` | ✅ | Buffer accumulation |
| `bun:sqlite` | ❌ | Use Cloudflare D1 |
| `bun:ffi` | ❌ | Use WASM |
| `bun:test` | ❌ | Use `bun test` directly |

## Build

```bash
cd src/bun
bun run build.ts
```

Output: `dist/bun/bun-bundle.js`

## Test

### Unit Tests

```bash
cd src/bun
bun test
```

Expected: 198 pass, 51 skip (network/integration tests)

### Manual Testing with Workerd

1. Build the bundle:
```bash
cd src/bun && bun run build.ts
```

2. Build the sample worker:
```bash
cd samples/helloworld-bun
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

3. Create a local config:
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
```

4. Run workerd:
```bash
workerd serve config-local.capnp
```

5. Test with curl:
```bash
curl http://localhost:9124/
curl "http://localhost:9124/hash?data=hello"
curl http://localhost:9124/deep-equals
curl http://localhost:9124/stream
curl http://localhost:9124/health
```

## Limitations

- **File Operations**: Requires `nodejs_compat` flag
- **Password Hashing**: PBKDF2 only (not bcrypt/argon2 compatible)
- **Hash Functions**: JS implementations, not bit-identical to native Bun
- **SQLite**: Not available - use Cloudflare D1

## License

Apache 2.0
