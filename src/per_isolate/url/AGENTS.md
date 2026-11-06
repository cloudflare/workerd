# src/per_isolate/url/

`URL` and `URLSearchParams` in TypeScript, installed by `main.ts` under the
`TS_URL` autogate in place of both C++ implementations (`api/url-standard.h`
and `api/url.h`), which `global-scope.h` then leaves out. Parent directory
conventions apply; see `src/per_isolate/AGENTS.md`.

## MODULES

| Module | Role |
| --- | --- |
| `backend.ts` | The facade-backend contract (`UrlBackend`, `SearchParamsStore`); exports the attribute numbers |
| `url.ts` | The facade: both classes, the iterator, every WebIDL conversion, the shape; picks the backend |
| `ada-backend.ts` | `url_standard`: href plus ada's offsets, sliced by the getters; `ListStore` |
| `kj-backend.ts` | `url_original`: adapts the native `LegacyUrlHandle` (`api/url.h`) |
| `urlencoded.ts` | application/x-www-form-urlencoded parsing and serializing, for `ListStore` |
| `internal-array.ts` | Null-prototype arrays for internal lists |
| `cpp_exports.ts` | The functions the C++ bridge calls |

The graph is acyclic: `url` requires `backend`, one backend, `runtime-class`
and `serialization`; the backends require only `backend`, `urlencoded` and
`internal-array`.

## SEMANTICS

`src/tests/url/AGENTS.md` describes the behavior and keeps the ledger of
every departure from the C++, toward WHATWG URL and WebIDL. Its suite runs
against both implementations; change the code and the ledger together.

- The backend is chosen once per context from `url_standard`. It decides
  the parser and every attribute's value; the facade decides everything
  else, the same way for both.
- The facade converts every argument before reaching the backend, so a
  backend sees only USVStrings.
- `URL.parse()` and the `searchParams` getter hand their constructors an
  already-made state through `adoptedUrlState` / `adoptedStore`, set just
  before the `new`.

## INVARIANTS

- **Origin cache** (`ada-backend.ts`): only the `href`, `protocol`, `host`,
  `hostname` and `port` setters clear it. `src/tests/url/origin-cache.js`
  checks it after every setter; a new setter path must keep that rule.
- **A URL's store is its query.** Every `ListStore` mutation reserializes
  the URL's query, even one that changes nothing; the `href` and `search`
  setters reload the list. The kj backend's handle is both URL and store.
- **Internal lists** are made with `internalArray()`: a push onto an
  ordinary array runs any index setter user code put on `Array.prototype`.
  Return them to user code through `toUserArray()`.
- Every instance, iterators included, is `markUncloneable()`.

## THE C++ CONTRACT

`api/url-bridge.h` defines `JsUrl` and `JsUrlSearchParams`, which take the
TypeScript instances where C++ took the C++ classes: `node:fs` paths
(`FileSystemModule::FilePath`) and body initializers (`Body::Initializer`).

- They recognize an instance by the V8-private brand its constructor sets
  (`workerd:URL`, `workerd:URLSearchParams`), without running JavaScript.
- They read it through `cpp_exports.ts` (`urlHref`,
  `searchParamsSerialize`), which uses the implementation's own state, so
  user code replacing `toString()` does not change what C++ sees.
- The brand names and the export names are shared with `url-bridge.c++`.
