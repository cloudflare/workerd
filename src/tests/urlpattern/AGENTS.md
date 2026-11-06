# URLPattern

An informal specification of URLPattern as implemented in workerd, kept in
lockstep with the test suite in this directory. **The tests are the
normative artifact.** The cells run the same modules
(`urlpattern-modules.capnp`) against the C++ implementations and the
TypeScript one:

| Cell | Implementation | Flags the suite branches on |
| --- | --- | --- |
| `urlpattern-standard-cpp` | `api/urlpattern-standard.{h,c++}` (ada) | `urlpattern_standard`, `set_tostring_tag` |
| `urlpattern-legacy-cpp` | `api/urlpattern.{h,c++}` (`jsg::UrlPattern`) | `urlpattern_original`, `do_not_set_tostring_tag` |
| `urlpattern-opt-in-cpp` | standard, with the dateless WebIDL opt-ins | standard + `spec_compliant_property_attributes`, `immutable_api_prototypes` (`--experimental`) |
| `urlpattern-standard-ts`, `urlpattern-legacy-ts` | `src/per_isolate/urlpattern/`, under the `TS_URL_PATTERN` autogate | as the C++ cell of the same name |

`which-impl.js` reads the flags; a cell's `USING_TS_IMPL` binding says which
implementation it means to run. `enable_fast_jsg_struct` is not pinned: the
`@` and `@all-compat-flags` variants run each cell with it off and on.

The TypeScript implementation honors the dated flags above and is otherwise
WebIDL-conformant, so it differs from the C++ only toward the spec, in the
rows of the ledger below. Each row is pinned on both sides with a
`usingTsImpl` branch. The C++ cells disable the `@all-autogates` variant,
which would run the TypeScript implementation under them.

## Core semantics

- **Parsing:** the standard parser is ada, the legacy one
  `jsg::UrlPattern`; their patterns, quirks included, are pinned as they
  are (`construction.js`). In a pattern string `:` starts a named group and
  `?` makes the part before it optional. The standard parser leaves the
  components after the last one a string names as `*`, and against a
  baseURL the credentials too; the legacy one makes them `''`, and takes a
  baseURL's search and hash as well.
- **Errors:** every standard failure is `TypeError: Failed to construct
  URLPattern` (or `Failed to exec URLPattern` / `Failed to test URLPattern`);
  the legacy parser's messages say what failed.
- **RegExp flags:** `v` (standard) and `u` (legacy), plus `i` under
  `ignoreCase`.
- **Inputs:** an object, `null` or `undefined` is a `URLPatternInit`
  (a URL included, read through its getters); anything else a string. A
  string input is parsed as a URL; one that does not parse, or a baseURL
  that does not, does not match. An init input is processed as URL
  components; when that fails the standard parser does not match and the
  legacy one throws its message. An init with a baseURL throws.
- **Results:** `inputs` holds the string input and baseURL, or a copy of
  the init. Groups are named by the pattern's names, wildcards and regexp
  groups numbered; an unmatched optional group is `undefined` (standard) or
  `''` (legacy).
- **Constructor overloads:** with two arguments, a second that is an
  object, `null` or `undefined` is the options, anything else the baseURL.
- **Serialization:** a URLPattern throws `DataCloneError: Could not
  serialize object of type "<constructor>".`; results are plain objects.

## Ledger

| # | Area | C++ | TypeScript | Test |
| --- | --- | --- | --- | --- |
| 1 | Standard string input that does not parse, no baseURL | ada resolves it against an empty URL: `test('nope')` is true | no match | `matching.js` |
| 2 | Standard group order | ada's `std::unordered_map` order | the pattern's | `results.js` |
| 3 | `URLPatternInit` read order | declared (`protocol` … `hash`, `baseURL`) | lexicographic | `conversions.js` |
| 4 | Result and `inputs` copy member order | declared (`inputs`, `protocol` …; `input`, `groups`) | lexicographic | `results.js` |
| 5 | Patched `RegExp.prototype.exec` | called for each component ada's fast paths do not cover; legacy all eight | not called | `pollution.js` |
| 6 | Legacy lone surrogates | WTF-8 | U+FFFD | `conversions.js` |
| 7 | Three arguments | an object or `undefined` second argument is the options (`undefined`: no baseURL); the third is read and ignored | the second is the baseURL (`ToUSVString`) | `construction.js`, `conversions.js` |
| 8 | Accessor names, constructor `prototype`, `Symbol.toStringTag`, global | JSG: anonymous, writable, writable, on the global's prototype (WebIDL under the opt-ins but the tag) | WebIDL | `api-surface.js` |
| 9 | Calling without `new` | JSG's TypeError message | the language's | `construction.js` |
| 10 | `Object.prototype` setters | groups always set, so a setter takes them; without `enable_fast_jsg_struct` results assigned too | data properties | `pollution.js` |
| 11 | `inputs` copy of an init under `enable_fast_jsg_struct` | every member, those absent `undefined` | the members present | `results.js` |
