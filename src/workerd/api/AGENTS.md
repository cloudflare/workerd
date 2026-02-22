# src/workerd/api/

All JavaScript APIs exposed to Workers: HTTP, crypto, streams, WebSocket, Cache, KV, R2, SQL, encoding, events, timers, scheduled/alarm handlers.

## STRUCTURE

```
*.h + *.c++          # Top-level API impls (http, headers, cache, blob, url, events, etc.)
crypto/              # WebCrypto + Node crypto: AES, RSA, EC, DH, HKDF, PBKDF2, X.509, JWK
streams/             # ReadableStream/WritableStream/TransformStream (internal + standard); has README.md
node/                # Node.js compat C++ layer; register new modules in NODEJS_MODULES macro in node.h
pyodide/             # Python Workers Pyodide/Emscripten bridge (7 files)
tests/               # JS integration tests (238 entries); each test = .js + .wd-test pair
```

## WHERE TO LOOK

| Task                           | Files                                                 |
| ------------------------------ | ----------------------------------------------------- |
| fetch / Request / Response     | `http.h`, `http.c++`                                  |
| Headers                        | `headers.h`, `headers.c++`                            |
| WebSocket                      | `web-socket.h`, `web-socket.c++`                      |
| Hibernatable WS (DO)           | `hibernatable-web-socket.h`                           |
| GlobalScope / addEventListener | `global-scope.h`, `global-scope.c++`, `basics.h`      |
| JS RPC / Fetcher               | `worker-rpc.h`, `worker-rpc.c++`                      |
| Durable Object state           | `actor-state.h`, `actor.h`                            |
| Streams internals              | `streams/` (see `streams/README.md` for architecture) |
| Crypto (WebCrypto)             | `crypto/crypto.h`, `crypto/impl.h`, `crypto/keys.h`   |
| Node.js compat (C++)           | `node/node.h` (NODEJS_MODULES macro), per-module .h   |
| URL (legacy vs standard)       | `url.h` (legacy), `url-standard.h` (spec-compliant)   |
| Encoding (TextEncoder/Dec)     | `encoding.h`; stream variants in `streams/encoding.h` |
| Scheduled / Queue / Alarm      | `scheduled.h`, `queue.h`, `actor-state.h`             |
| HTMLRewriter                   | `html-rewriter.h`                                     |
| R2 storage                     | `r2-bucket.h`, `r2-admin.h`, `r2-multipart.h`         |
| KV / SyncKV                    | `kv.h`, `sync-kv.h`                                   |
| SQL (DO)                       | `sql.h`                                               |
| Body mixin (shared Req/Res)    | `http.h` — `Body` class, `Body::Initializer` OneOf    |

## CONVENTIONS

- `global-scope.h` forward-declares most API classes; `ServiceWorkerGlobalScope` registers all nested types
- Node.js compat: add C++ class + register in `NODEJS_MODULES(V)` macro in `node/node.h`; experimental modules go in `NODEJS_MODULES_EXPERIMENTAL(V)`
- URL has dual impl: legacy (`url.h`) vs standard (`url-standard.h`); compat flag selects which
- Streams has dual impl: internal (`streams/internal.h`) vs standard (`streams/standard.h`)
- Test naming: `tests/<feature>-test.js` + `tests/<feature>-test.wd-test`; C++ unit tests: `<feature>-test.c++`
