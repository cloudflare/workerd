# Module Fallback Redirect Reproduction

This reproduces a V2 module fallback failure involving a redirected module that
performs a dynamic import. It does not use Vite or Workers SDK.

Start the fallback service from the repository root:

```sh
node samples/module-fallback-redirect/fallback.mjs
```

In another terminal, start Workerd:

```sh
npx workerd@latest serve samples/module-fallback-redirect/config.capnp --experimental
```

Then invoke the Worker:

```sh
curl http://127.0.0.1:8080
```

The Worker imports two bare dependencies. The fallback service redirects both
to canonical `file:///project/node_modules/...` URLs. The `/project` path is a
logical, portable stand-in for a resolved project path; this reproduction does
not read the host filesystem.

- `static-test-dependency` statically imports `./value.mjs`.
- `dynamic-test-dependency` dynamically imports `./value.mjs` when `load()` is
  called.

If both imports succeeded, Workerd would log:

```text
static import: loaded
dynamic import: loaded
```

Instead, the static import succeeds, but the dynamic import returns HTTP 500
with:

```text
Referring module not found in the registry: file:///project/node_modules/dynamic-test-dependency/index.mjs
```
