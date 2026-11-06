# URL and URLSearchParams

An informal specification of URL and URLSearchParams as implemented in
workerd, kept in lockstep with the test suite in this directory. **The tests
are the normative artifact.** The cells run the same modules
(`url-modules.capnp`) against the C++ implementations:

| Cell | Implementation | Flags the suite branches on |
| --- | --- | --- |
| `url-standard-cpp` | `api/url-standard.{h,c++}` (ada) | `url_standard`, `urlsearchparams_delete_has_value_arg`, `set_tostring_tag` |
| `url-standard-legacy-flags-cpp` | the same | `url_standard`, `no_urlsearchparams_delete_has_value_arg`, `do_not_set_tostring_tag` |
| `url-legacy-cpp` | `api/url.{h,c++}` (`kj::Url`) | `url_original`, `workers_api_getters_setters_on_prototype`, `set_tostring_tag`, `urlsearchparams_delete_has_value_arg` |
| `url-legacy-instance-cpp` | the same, attributes on each instance | `url_original`, `workers_api_getters_setters_on_instance`, `do_not_set_tostring_tag`, `no_urlsearchparams_delete_has_value_arg` |
| `url-opt-in-cpp` | standard, with the dateless WebIDL opt-ins | standard + `spec_compliant_property_attributes`, `immutable_api_prototypes` (`--experimental`) |

`which-impl.js` reads the flags; a cell's `USING_TS_IMPL` binding says which
implementation it means to run. `enable_fast_jsg_struct` is not pinned: the
`@` and `@all-compat-flags` variants run each cell with it off and on.

A TypeScript implementation (`src/per_isolate/url/`, behind the `TS_URL`
autogate) is planned. It honors the dated flags above and is otherwise
WebIDL-conformant, so it may differ from the C++ only toward the spec, in the
rows of the ledger below. Each row is pinned on both sides with a
`usingTsImpl` branch; the TS branches describe the target, and are exercised
once the TS cells exist.

## Core semantics

- **Parsing:** the standard backend is ada; the legacy backend is `kj::Url`,
  whose results, quirks included, are pinned as they are (`setters.js`,
  `construction.js`). Legacy constructor errors distinguish `Invalid base
  URL string.`; the standard one says `Invalid URL string.` for everything.
- **Setters:** only `href` throws (`Failed to set the 'href' property on
  'URL': ...`); the others leave the URL unchanged on a value they reject.
  The legacy `host`, `hostname` and `port` setters do not validate, and a
  host `kj::Url` cannot stringify makes `href`, `toString()` and `toJSON()`
  throw `Invalid hostname when stringifying URL.`
- **searchParams** is the same object for the URL's lifetime. The `href`
  and `search` setters reach it, and every mutator, even one that changes
  nothing, reserializes the URL's query (`?a=b%20c` becomes `?a=b+c`).
- **Arguments:** a missing required argument is a TypeError; an explicit
  `undefined` converts to `"undefined"`, `null` to `"null"`. The standard
  path converts to USVStrings (a lone surrogate becomes U+FFFD); the legacy
  one keeps the WTF-8 bytes.
- **Iteration** is live by index: deleting the current entry skips the next,
  appended entries are visited, and `done` is not sticky.
- **Serialization:** every URL, URLSearchParams and iterator throws
  `DataCloneError: Could not serialize object of type "<constructor>".`

## Ledger

| # | Area | C++ | TypeScript | Test |
| --- | --- | --- | --- | --- |
| 1 | Legacy instance layout | 12 own native data properties, reverse WebIDL order | own accessors, same order | `api-surface.js` |
| 2 | Constructors' `prototype` | writable unless `immutable_api_prototypes` | non-writable | `api-surface.js` |
| 3 | Calling without `new` | JSG's TypeError message | the language's | `construction.js` |
| 4 | Serialization message | names the constructor | the same | `serialization.js` |
| 4a | Iterator serialization message | `EntryIterator`, ... | follows #6 | `serialization.js` |
| 5 | Member order | standard: `origin` first, `searchParams` after `hash`; legacy: WebIDL attributes, `toString` before `toJSON` | WebIDL | `api-surface.js`, `inspect.js` |
| 6 | Iterators | three prototypes and tags (`EntryIterator`, ...), `@@iterator !== entries` | one `URLSearchParams Iterator` prototype, `@@iterator === entries` | `iteration.js`, `api-surface.js`, `illegal-invocation.js` |
| 9 | Iterator results and `Object.prototype` setters | without `enable_fast_jsg_struct`, made by assignment: `done`/`value` setters run | object literals | `pollution.js` |
| 13 | Function lengths, accessor names | 0 and anonymous, unless `spec_compliant_property_attributes` | WebIDL | `api-surface.js` |
| 14 | Globals | on the global's prototype; also own under `spec_compliant_property_attributes` | own only | `api-surface.js` |
| 15 | Legacy statics | none | `canParse`, `parse`, `createObjectURL`, `revokeObjectURL` | `api-surface.js`, `construction.js` |
| 16 | Legacy initializer | arrays and dicts: a `Map` or other iterable is a dict of its own properties | WebIDL union | `search-params.js` |
| 17 | Legacy lone surrogates | WTF-8 | U+FFFD | `conversions.js` |
| 18 | `forEach(cb, null)` receiver | `undefined` | `null` | `iteration.js` |
| 20 | `Symbol.toStringTag` | writable | non-writable | `api-surface.js` |
| 21 | Initializer with a non-callable `@@iterator` | a record | TypeError | `search-params.js` |
| 22 | Legacy-instance `URL.prototype` | `toString`, `toJSON` only | the accessors too | `api-surface.js` |
| 23 | Legacy `delete`/`has` value under `urlsearchparams_delete_has_value_arg` | ignored | honored | `search-params.js` |
| 24 | Iterator result shape | `{ done, value }`; without `enable_fast_jsg_struct` the last lacks `value` | `{ value, done }` | `iteration.js` |

Rows the plan keeps but no test here covers: #7 and #11 (now matches), #8
and #19 (WPT), #10 (RPC; with the TS cells) and #12 (heap snapshots).
