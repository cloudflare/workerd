# src/per_isolate/urlpattern/

`URLPattern` in TypeScript, installed by `main.ts` under the
`TS_URL_PATTERN` autogate in place of both C++ implementations
(`api/urlpattern-standard.h` and `api/urlpattern.h`), which `global-scope.h`
then leaves out. Parent directory conventions apply; see
`src/per_isolate/AGENTS.md`.

## MODULES

| Module | Role |
| --- | --- |
| `urlpattern.ts` | The class: WebIDL conversions and overloads, matching, results, the shape |

It requires `url/backend`, `url/ada-backend` (string inputs are parsed with
ada, whichever parser compiled the pattern), `runtime-class` and
`serialization`; nothing requires it but `main.ts`.

## SEMANTICS

`src/tests/urlpattern/AGENTS.md` describes the behavior and keeps the ledger
of every departure from the C++, toward the URLPattern standard and WebIDL.
Its suite runs against both implementations; change the code and the ledger
together.

- The parser is chosen once per context from `urlpattern_standard`: utils'
  `urlPatternCompile` / `urlPatternProcessInit` (ada) or the `legacy*` pair
  (`jsg::UrlPattern`), in `api/urlpattern-bootstrap.h`. It decides every
  pattern, the components an init input processes to, the RegExp flags and
  the errors. Everything else is here, the same for both.
- **Compile natively, match here.** A compiled component is its pattern
  string, its RegExp, its group names and ada's fast-path type
  (`FULL_WILDCARD`, `EXACT_MATCH`, `EMPTY`, else the RegExp); legacy
  components are always RegExps. A string input needs no native call beyond
  `utils.urlParse`.
- Matching uses `RegExpPrototypeExec`, the builtin, so a patched
  `RegExp.prototype.exec` is never called.
- Nothing matching makes runs a setter user code put on `Object.prototype`
  or `Array.prototype`. Results and groups are object literals (computed
  keys for groups; `ObjectDefineProperty` past three groups, and for init
  copies), which define their members. Internal arrays are pre-sized
  literals, whose elements are their own, rather than `internalArray()`s,
  whose prototype change costs a runtime call each.

## INVARIANTS

- The compiled RegExps never reach user code: their `lastIndex` stays 0,
  which the builtin exec reads.
- Every instance is `markUncloneable()`. Nothing native consumes an
  instance, so there is no V8-private brand; the brand check is `#pattern`.
