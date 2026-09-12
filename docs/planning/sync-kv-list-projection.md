# Spec: `projection` option for synchronous DO KV `list()`

Status: proposed, not started.
Scope: `ctx.storage.kv.list()` on SQLite-backed Durable Objects.

## 1. Summary

Add an option to the synchronous KV `list()` operation that selects which parts of each record
the iterator yields:

```js
kv.list({ projection: 'keys' }); // yields string
kv.list({ projection: 'values' }); // yields T
kv.list({ projection: 'entries' }); // yields [string, T]  (default)
```

The primary motivation is `"keys"`: listing a key range without paying to deserialize (or, at the
SQL layer, to read) the stored values.

The work splits into two phases, broken into independently landable steps in §10:

- **API phase** (TODO steps 1–4). Plumb the option through the JS API layer, with types, tests and
  docs. Behaviour complete, no SQLite changes.
- **Key-only SQL** (TODO step 5, designed in §5.5). Prepared statements that never read the value
  blob off disk. In scope — large stored values are a confirmed workload.

The API phase captures the win that applies to every caller: skipping V8 structured-clone
deserialization per row. The key-only SQL step adds the I/O win for large values and removes a
per-row `_cf_EXTERNALS` query for values carrying externals.

The two phases are sequenced API-first deliberately. Step 5 is invisible to the API — same
options, same yielded shapes — so the feature can ship and be used before the storage-layer
optimization exists, and the optimization can then land or revert on its own.

## 2. Goals and non-goals

Goals:

- `list({projection: "keys"})` yields plain key strings and never deserializes a stored value.
- `list({projection: "values"})` yields plain values.
- Default behaviour is byte-for-byte unchanged: `list()` and `list({projection: "entries"})` yield
  `[key, value]` pairs.
- No new compatibility flag. (See §6 for the reasoning; the decision must be recorded in the
  commit message.)
- TypeScript users get the correct element type for each projection.

Non-goals:

- The **asynchronous** `storage.list()` API is not changed. Decided, not merely deferred: it
  returns a `Map` via `ActorCacheOps`, whose LRU cache is value-oriented, so a keys-only path
  there raises cache-population and read-your-writes questions that this feature does not need to
  answer. The resulting sync/async divergence is accepted and must be stated in the public docs.
- No change to `list()`'s single-cursor-at-a-time restriction.
- No change to billing. Usage is metered as SQLite rows read
  (`SqliteKvRegulator::shouldAddQueryStats()`, `src/workerd/util/sqlite.h:41`), and the row count
  is identical under every projection.

## 3. Context: how `kv.list()` works today

### 3.1 Call chain

```
JS:  ctx.storage.kv.list(options)
 │
 ├─ api::SyncKvStorage::list()                            src/workerd/api/sync-kv.c++:32
 │   ├─ opens a user trace span, sets per-option tags
 │   ├─ converts SyncKvStorage::ListOptions
 │   │      → DurableObjectStorageOperations::ListOptions  (drops allowConcurrency/noCache)
 │   ├─ DurableObjectStorageOperations::compileListOptions()   api/actor-state.c++
 │   │      normalizes {start, startAfter, end, prefix} → {start, end, reverse, limit};
 │   │      returns kj::none if the range is provably empty
 │   ├─ SqliteKv::list(begin, end, limit, order) → kj::Own<ListCursor>
 │   │                                                     util/sqlite-kv.c++:93
 │   └─ js.alloc<ListIterator>(IoContext::addObject(cursor))
 │
 └─ per `next()`: api::SyncKvStorage::listNext()           src/workerd/api/sync-kv.c++:89
     ├─ ListCursor::next() → KeyValuePair{StringPtr key, ArrayPtr<const byte> value}
     └─ deserializeV8Value(js, key, value)                 src/workerd/io/stored-value.c++
```

The iterator is declared as:

```cpp
JSG_ITERATOR_TYPE(ListIterator, jsg::JsArray, IoOwn<SqliteKv::ListCursor>, listNext);
```

`jsg::JsArray` is the yielded element type; the state is the cursor. `jsg::IteratorBase` wraps the
yielded value in a transient `Next { bool done; Optional<Type> value; }` JSG_STRUCT
(`src/workerd/jsg/iterator.h:729`).

### 3.2 Storage layer

`_cf_KV` is declared (`util/sqlite-kv.c++:69`):

```sql
CREATE TABLE IF NOT EXISTS _cf_KV (
  key TEXT PRIMARY KEY,
  value BLOB
) WITHOUT ROWID;
```

`SqliteKv::Initialized` holds eight prepared list statements — the cross product of
{bounded, unbounded} × {limited, unlimited} × {forward, reverse} — all of the form
`SELECT * FROM _cf_KV WHERE key >= ? [AND key < ?] ORDER BY key [DESC] [LIMIT ?]`
(`util/sqlite-kv.h:118-161`). `ListCursor` reads column 0 as the key and column 1 as the value.

The eight-statement explosion is deliberate. Collapsing them with `(?2 IS NULL OR key < ?2)` and
`LIMIT ifnull(?3, -1)` would compile, but a disjunctive term cannot be used as an index range
constraint, so SQLite would scan from `start` to the end of the table and filter. That is a
significant regression for prefix listing on a large object. Do not "simplify" these.

`SqliteDatabase::prepare()` compiles eagerly; the lazily-recompiled `kj::String` alternative in
`Statement::stmt` exists only to survive a database reset
(`util/sqlite.c++:1476-1504`). So every statement in `Initialized` costs a
`sqlite3_prepare_v2()` at first-write time. This matters for the key-only SQL step (§5.5).

### 3.3 Cursor lifetime

Only one live cursor is allowed. `ListCursor`'s constructor calls `parent.cancelCurrentCursor()`,
which clears the previous cursor's state and sets `canceled = true`; the JS layer turns that into
a user-visible error on the next `next()` (`api/sync-kv.c++:93`). None of this changes.

### 3.4 Test coupling (read this before writing tests)

`src/workerd/api/tests/sync-kv-test.js` exercises `MyActor.run()`. The same worker has a
streaming tail worker, `sync-kv-instrumentation-test.js`, which asserts the **exact ordered list**
of every `kv` span produced by that run. Adding a single `kv.list()` call to `sync-kv-test.js`
breaks the instrumentation test unless a matching span object is appended in the same commit.

## 4. Options considered

Criteria, in priority order: (a) does not change existing behaviour; (b) static type of the
iterator element is knowable in TypeScript; (c) leaves room for the storage-layer optimization;
(d) idiomatic for this codebase and for JS; (e) implementation cost.

### Option A — `projection: "keys" | "values" | "entries"` (recommended)

One option field on the existing options bag; the iterator's element type varies with it.

- (a) Yes, `"entries"` is the default.
- (b) Expressible with overloads discriminated on a string-literal option, exactly as
  `ReadableStream.getReader({mode: "byob"})` already does
  (`api/streams/readable.h:324-334`, generated output at
  `types/generated-snapshot/index.d.ts:2749-2755`). Caveat: a caller whose `projection` is a
  *variable* of the union type matches no specific overload and falls through to the `"entries"`
  overload, receiving a wrong element type. Documented limitation, not a soundness hole in the
  runtime.
- (c) Yes — `list()` chooses the SQL statement, and the projection is known at that point.
- (d) `jsg::Optional<kj::String>` + explicit validation + a string-literal union in the TS
  override is an established pattern here: `UnsafeModule::EvictOptions::webSockets`
  (`api/unsafe.h:109-116`, validated in `api/unsafe.c++:29-36`),
  `ReadReplicationOptions::mode` (`api/actor-state.h:715`), `ResponseInit::encodeBody`
  (`api/http.h:1055-1058`).
- (e) Low.

### Option B — separate methods: `kv.listKeys()` / `kv.listValues()` / `kv.list()`

- (b) Strictly better: each method has exactly one element type, no overload resolution to get
  wrong, and a dynamically chosen mode becomes an explicit `if`.
- (d) Matches `Map`/`Headers`/`URLSearchParams`/`FormData`
  (`keys()`/`values()`/`entries()`), which is the JS-idiomatic spelling, and matches
  `URLSearchParams`' three `JSG_ITERATOR` types in this codebase (`api/url-standard.h:60-71`).
- Cost: three methods with three duplicated options blocks and three iterator types in
  `EW_SYNC_KV_ISOLATE_TYPES`; the surface grows permanently and cannot be un-shipped.
- Rejected: it is not what was asked for, and the deciding argument for (d) is weaker than it
  looks — `list()` is already an options-bag API rather than an `entries()`, so the "web
  idiomatic" pull is mostly aesthetic. Option A's TS caveat is the real cost, and it is
  documentable.

### Option C — `keysOnly: true`

Rejected. Boolean argument (against the repo convention, see `util/strong-bool.h`), cannot
express values-only, and leaves no room for a future `"metadata"` projection.

### Option D — projection on the iterator: `kv.list(opts).keys()`

Rejected. `list()` creates the cursor — and cancels any previous one — before `.keys()` is
called, so the SQL statement is already chosen and the §5.5 optimization becomes impossible
without making `list()` lazy. Making it lazy would also move the cancellation point, which is
observable behaviour.

## 5. Recommended design

### 5.1 JS-visible semantics

| `projection` | yields         | value deserialized? | value read from SQLite? (after §5.5)   |
| ------------ | -------------- | ------------------- | -------------------------------------- |
| absent       | `[key, value]` | yes                 | yes                                    |
| `"entries"`  | `[key, value]` | yes                 | yes                                    |
| `"keys"`     | `key`          | no                  | no                                     |
| `"values"`   | `value`        | yes                 | yes                                    |

`"entries"` is named after `Map.prototype.entries()`, which yields the same `[key, value]` shape.

Anything else throws `TypeError`:
`options.projection must be "keys", "values", or "entries".`

Note that `"values"` is an ergonomic convenience only. The key is free to read (it *is* the
`WITHOUT ROWID` b-tree key) and `deserializeV8Value()` needs it for error messages and for the
externals handler, so the only saving is one two-element JS array allocation per row. Do not
advertise `"values"` as a performance feature.

`"keys"` has a second, less obvious benefit: a record whose stored value fails to deserialize
cannot break a keys-only listing, because the deserializer never runs.

### 5.2 `src/workerd/api/sync-kv.h`

```cpp
class SyncKvStorage: public jsg::Object {
 public:
  // Selects which parts of each record list() yields.
  enum class Projection {
    ENTRIES,  // [key, value]
    KEYS,     // key
    VALUES,   // value
  };

  struct ListOptions {
    jsg::Optional<kj::String> start;
    jsg::Optional<kj::String> startAfter;
    jsg::Optional<kj::String> end;
    jsg::Optional<kj::String> prefix;
    jsg::Optional<bool> reverse;
    jsg::Optional<int> limit;
    jsg::Optional<kj::String> projection;

    JSG_STRUCT(start, startAfter, end, prefix, reverse, limit, projection);
    JSG_STRUCT_TS_OVERRIDE(SyncKvListOptions {  // Rename from SyncKvStorageListOptions
      projection?: "keys" | "values" | "entries";
    });
  };

 private:
  struct ListState {
    IoOwn<SqliteKv::ListCursor> cursor;
    Projection projection;
  };

 public:
  JSG_ITERATOR_TYPE(ListIterator, jsg::JsValue, ListState, listNext);
  ...
 private:
  static Projection parseProjection(jsg::Optional<kj::String>& projection);
  static kj::Maybe<jsg::JsValue> listNext(jsg::Lock& js, ListState& state);
};
```

Three things to note:

- **The iterator's element type must widen from `jsg::JsArray` to `jsg::JsValue`.** This is forced
  by the macro. `JSG_ITERATOR_TYPE` bakes a single `Type` into `jsg::IteratorBase`, where it is
  simultaneously the return type of the next function and the static type of the `value` property
  of every result object (`jsg/iterator.h:727-733`):

  ```cpp
  using NextSignature = kj::Maybe<Type>(Lock&, State&);
  struct Next { bool done; Optional<Type> value; JSG_STRUCT(done, value); };
  ```

  There is exactly one `Type` per iterator class. The three projections yield `JsString`,
  `JsValue` and `JsArray`; `jsg::JsValue` is the narrowest type all three fit into, via
  `JsBase::operator JsValue()` (`jsg/jsvalue.h:195`). Alternatives are in Appendix A.

  The widening costs nothing on the safety or types axes. The repo's "no `JsValue` in a
  `JSG_STRUCT` field" rule already applies to `Next` via `JsArray` today — both are `v8::Local`
  wrappers hitting the same debug-only `requireOnStack` assertion (`jsg/jsvalue.h:14-18`,
  `jsvalue.c++:23`) — and the rule exists because JSG_STRUCTs can be *stored*. A synchronous
  iterator's `Next` is constructed and wrapped within one `next()` call and never escapes;
  `SqlStorage::Cursor::RowIterator` does the same with `jsg::JsObject`. (The asynchronous
  `ReadableStream` iterator uses `jsg::V8Ref<v8::Value>` precisely because it *does* cross a
  promise boundary.) There is no TS snapshot impact either, for the reason given in §5.4. Put
  this in the commit message so a reviewer does not have to re-derive it.

  The one genuine loss is self-documentation: `JsArray` used to tell a reader "this yields
  pairs". That is why `Projection` is stored explicitly in `ListState` rather than being
  inferred at the yield site.
- `ListState` has no GC-visitable fields, so `IteratorBase::visitForGc` continues to visit
  nothing (it probes with `hasPublicVisitForGc<State>()`), matching today's bare-`IoOwn` state.
- `ListState` must be declared before the `JSG_ITERATOR_TYPE` that names it, and
  `EW_SYNC_KV_ISOLATE_TYPES` is unchanged.

### 5.3 `src/workerd/api/sync-kv.c++`

```cpp
SyncKvStorage::Projection SyncKvStorage::parseProjection(
    jsg::Optional<kj::String>& projection) {
  KJ_IF_SOME(p, projection) {
    if (p == "entries") return Projection::ENTRIES;
    if (p == "keys") return Projection::KEYS;
    if (p == "values") return Projection::VALUES;
    JSG_FAIL_REQUIRE(TypeError,
        "options.projection must be \"keys\", \"values\", or \"entries\".");
  }
  return Projection::ENTRIES;
}
```

In `list()`, **parse the projection before the options are moved**. The existing code does

```cpp
auto asyncOptions = kj::mv(maybeOptions).map([&](ListOptions&& options) { ... });
```

which leaves `maybeOptions` moved-from, and `compileListOptions()` may move further strings out.
Read `projection` while the options are still intact, and do so before the empty-range early
return, so that an invalid projection throws even when the key range is provably empty. That
ordering is behaviour, not style: it decides whether `list({end: "a", start: "b", projection: "nope"})`
throws.

Trace tag, emitted only when the option was supplied (matching how the other tags behave):

```cpp
traceContext.setTag("cloudflare.durable_object.kv.query.projection"_kjc, <ConstString>);
```

Map the enum to a `kj::ConstString` literal (`"keys"_kjc` etc.). Do **not** pass an allocated
string: commits `b467d7661` and `3cfcb78eb` exist specifically to remove `setTag()` string
allocations from this file.

`listNext` becomes:

```cpp
kj::Maybe<jsg::JsValue> SyncKvStorage::listNext(jsg::Lock& js, ListState& state) {
  auto& cursor = *state.cursor;
  if (state.projection == Projection::KEYS) {
    // The API phase uses next(); §5.5 switches this to cursor.nextKey()
    KJ_IF_SOME(pair, cursor.next()) {
      return js.str(pair.key);
    }
  } else {
    KJ_IF_SOME(pair, cursor.next()) {
      auto value = deserializeV8Value(js, pair.key, pair.value);
      if (state.projection == Projection::VALUES) return value;
      return js.arr(js.str(pair.key), value);
    }
  }
  if (cursor.wasCanceled()) {
    JSG_FAIL_REQUIRE(Error, "kv.list() iterator was invalidated ...");  // unchanged text
  }
  return kj::none;
}
```

The cancellation check must stay after the exhaustion check, exactly as today.

### 5.4 TypeScript

```cpp
JSG_TS_OVERRIDE({
  get<T = unknown>(key: string): T | undefined;

  list(options: SyncKvStorageListOptions & { projection: "keys" }): Iterable<string>;
  list<T = unknown>(options: SyncKvStorageListOptions & { projection: "values" }): Iterable<T>;
  list<T = unknown>(options?: SyncKvStorageListOptions): Iterable<[string, T]>;
  // The final overload covers both no-argument and explicit `projection: "entries"`.

  put<T>(key: string, value: T): void;

  delete(key: string): boolean;
});
```

Overload order matters — the two specific projections must precede the general signature. The
generated iterator types stay pruned from the snapshot because `list()`'s declared return type is
`Iterable<...>`, which is why `SyncKvStorageListIterator` does not appear in
`types/generated-snapshot/index.d.ts` today. Widening the C++ element type therefore has no
snapshot effect beyond these lines plus the new `projection` field on `SyncKvListOptions`.

### 5.5 Key-only SQL (TODO step 5)

In scope: large stored values are a confirmed workload, which is exactly the case where reading
the value to list keys is pure waste.

Two distinct wins, both avoided by never materializing the `value` column:

1. **Overflow-page reads.** SQLite materializes a column's content only when that column is read,
   so `SELECT key` does not walk the overflow-page chain of a large `value`. Note that `_cf_KV`
   is `WITHOUT ROWID`, so it is an *index* b-tree, whose maximum local payload is much smaller
   than a table b-tree's — on the order of a kilobyte at the default page size. Values spill to
   overflow sooner than they would in an ordinary table, so the optimization starts paying off at
   a lower value size than the `WITHOUT ROWID` layout might first suggest. Treat the exact
   crossover as something the benchmark determines, not something to assert; see
   https://www.sqlite.org/fileformat2.html for the record and overflow-page format.
2. **Externals lookups.** `deserializeV8Value()` installs a
   `StoredExternalHandler::Deserializer`, whose state is initialized lazily on the first external
   encountered in the value. That initialization issues
   `SELECT token FROM _cf_EXTERNALS WHERE key = ? ORDER BY idx`
   (`io/stored-value.c++:348-365`, statement at `util/sqlite-kv.h:182`) unless a pending write for
   the key is still in memory. So for values carrying externals, the entries and values
   projections cost **one additional SQL query per row**. Keys-only skips deserialization
   entirely and therefore skips those queries too. This win is independent of value size and
   applies as soon as any listed value references a stub or capability.

No covering index is needed: because the table is `WITHOUT ROWID`, the primary-key b-tree *is*
the table, so `SELECT key` is already served entirely from the structure being scanned.

Everything in this section is internal C++. `SqliteKv` and `SqliteKv::ListCursor` carry no JSG
macros and are not registered isolate types — `EW_SYNC_KV_ISOLATE_TYPES` contains only
`SyncKvStorage`, `ListOptions`, `ListIterator` and `ListIterator::Next` — so none of it reaches
JavaScript and no JS-visible result type changes here.

Beware the name collision: the JS iterator protocol method `ListIterator.next()`, generated by
`JSG_ITERATOR_TYPE` and returning `{done, value}`, is unrelated to the storage-layer
`ListCursor::next()` below. `JSG_ITERATOR_TYPE` emits only `JSG_METHOD(next)` and
`JSG_ITERABLE(self)`; `nextKey()` is reachable only from `listNext()` in C++.

Proposed shape, avoiding an overload-resolution hazard with the existing
`list(begin, end, limit, order, Func&& callback)` template — do *not* add a fifth positional
parameter to `list()`:

```cpp
// In SqliteKv:
enum ValueMode { WITH_VALUES, KEYS_ONLY };

// List keys in a range without reading the values. Cheaper than list() when values are large.
// The returned cursor supports nextKey() but not next().
kj::Own<ListCursor> listKeys(
    KeyPtr begin, kj::Maybe<KeyPtr> end, kj::Maybe<uint> limit, Order order);
```

and on `ListCursor`:

```cpp
// Advance and return the key only. Valid in either ValueMode.
kj::Maybe<KeyPtr> nextKey();

// Advance and return key and value. Requires the cursor to have been created by list(),
// not listKeys().
kj::Maybe<KeyValuePair> next();
```

`next()` and `nextKey()` share a private `advance()` helper for the `first`/`nextRow()` logic;
`next()` gains a `KJ_REQUIRE(mode == WITH_VALUES, ...)`. The empty-cursor constructor
(`ListCursor(decltype(nullptr))`) needs a mode too; give it a parameter rather than guessing, and
update the `createObject<SqliteKv::ListCursor>(nullptr)` call in `api/sync-kv.c++`.

To add eight key-only statements without doubling the hand-written statement list, group them:

```cpp
// The eight shapes of a list query: {bounded, unbounded} x {limited, unlimited} x
// {forward, reverse}. `columns` is spliced into the SELECT clause so the same eight shapes can
// be prepared for key-only listing.
struct ListStatements {
  SqliteDatabase::Statement plain, bounded, limited, boundedLimited;
  SqliteDatabase::Statement plainRev, boundedRev, limitedRev, boundedLimitedRev;

  ListStatements(SqliteDatabase& db, kj::StringPtr columns);
};
```

Two details that make this safe:

- `db.prepare()` does not retain the caller's SQL text — `Statement::beforeSqliteReset()` recovers
  it from `sqlite3_sql()` (`util/sqlite.c++:1502`). Building each statement from a temporary
  `kj::str(...)` is therefore fine.
- Hold the key-only group in a `kj::Maybe<ListStatements>` initialized on first use. Eager
  construction would add eight `sqlite3_prepare_v2()` calls to every DO session's first write,
  including the overwhelming majority that never list keys-only.

While restructuring, change `SELECT *` to an explicit `SELECT key, value`. `ListCursor` already
depends on the column order positionally; making it explicit is free.

## 6. Compatibility analysis

`SyncKvStorage` is exposed unconditionally — `JSG_LAZY_INSTANCE_PROPERTY(kv, getKv)` in
`api/actor-state.h:308` has no flag guard — and the feature is publicly documented. So this is a
change to a GA API surface, and the api-review rules apply.

Assessment: **no compatibility flag required.**

- `JSG_STRUCT` ignores unrecognized JS properties. A worker that passes `{projection: ...}` today
  gets it silently dropped and receives entries.
- The only new failure mode is that `projection: <not one of the three>` now throws `TypeError`
  instead of being ignored. That can only affect code that was already passing a nonexistent
  option with a bogus value.
- Every other option ever added to an existing options bag in this codebase carried the same
  theoretical exposure and shipped unflagged.

Record this decision explicitly in the commit message, per the repo's backward-compatibility
rules. If someone wants belt-and-braces, the only workable gate is a `FeatureFlags::get(js)`
check inside `list()` that rejects `projection` unless the flag is on — a `JSG_STRUCT` field
cannot be conditionally declared, because `JSG_STRUCT` takes no flags parameter. I do not
recommend it: it buys nothing against a risk this small and leaves a permanent wart.

## 7. Success criteria

1. `[...kv.list({projection: 'keys'})]` deep-equals the key array; `'values'` yields the values;
   `'entries'` and no-argument are identical to today's output.
2. Every existing assertion in `sync-kv-test.js` passes unmodified.
3. `projection: 'bogus'`, `projection: 42`, and `projection: null` throw `TypeError` with the
   documented message — including when the key range is empty.
4. Cursor invalidation still fires with the unchanged error message under every projection.
5. `just generate-types` produces a snapshot whose only diff is the new `projection` field and the
   three `list()` overloads; `test/types/` still type-checks.
6. `//src/workerd/api/tests:sync-kv-test@` passes in all three variants (`@`,
   `@all-compat-flags`, `@all-autogates`) and the instrumentation expectations match.
7. For the key-only SQL step (§5.5): a benchmark sweeps value size across at least
   256 B / 1 KiB / 8 KiB / 64 KiB at a fixed key count and reports wall time and pages read for
   keys-only versus entries listing. The goal is to locate the crossover point, not merely to
   show a win at one size — the public docs should be able to say at roughly what value size
   `"keys"` starts to matter. A separate case with externals-bearing values confirms the
   per-row `_cf_EXTERNALS` query disappears.

## 8. Trade-offs and risks

- **TypeScript overload fall-through.** A caller passing a `projection` variable typed as the full
  union gets the `"entries"` overload and thus the wrong element type. Inherent to Option A.
  Document it; recommend branching on the literal.
- **Runtime-varying return shape.** `list()` returning three different element types is harder to
  reason about than three methods would be. Mitigated by the precedent
  (`getReader`, `storage.get`, `storage.delete` all already do this) but it is a real cost.
- **Widening the iterator element type to `jsg::JsValue`** loses a compile-time guarantee that the
  yielded value is an array. Nothing depended on that guarantee — it never expressed the pair
  *shape*, only "is an array" — but it is a small loss of self-documentation, hence the explicit
  `Projection` enum on the state rather than inferring intent at the yield site. §5.2 records the
  one-element-array alternative and why it does not recover the guarantee.
- **`"values"` invites a wrong mental model.** Users will assume it is faster. It is not, in any
  way that matters. This is a documentation risk, not a code risk.
- **Key-only SQL (§5.5) doubles the prepared-statement surface** for list queries. Lazy
  initialization keeps the cost off the common path, but there is now more SQL to keep in sync,
  and the `next()`/`nextKey()` split introduces a mode that can be misused (guarded by
  `KJ_REQUIRE`).
- **Test coupling.** The instrumentation test's exact-span assertion makes `sync-kv-test.js` more
  expensive to extend than it looks. Budget for it.

## 9. Decisions taken and questions remaining

Decided:

- **Synchronous `storage.kv` only.** The asynchronous `storage.list()` gets no `projection`
  option. See §2.
- **`"entries"` is the default**, not `"all"`, matching `Map.prototype.entries()`.
- **Key-only SQL (§5.5) is in scope**, not speculative: large stored values are a confirmed
  workload. It still lands with benchmark numbers attached, to verify the implementation delivers
  and to locate the value size at which `"keys"` starts to matter.
- **All three projections ship**: `"keys"`, `"values"`, `"entries"`. `"values"` earns its place on
  symmetry with `Map`'s `keys()`/`values()`/`entries()` trio rather than on performance; §5.1 is
  explicit that it is an ergonomic convenience only. Whether to cut it is deliberately left open
  until just before the public docs land, since removing an accepted enum value after shipping is
  impossible.
- **The iterator element type widens to `jsg::JsValue`** and `"keys"` yields bare strings, so
  `[...kv.list({projection: "keys"})]` is `["bar", "foo"]`. Appendix A covers the alternatives.

Open:

1. **Public docs ownership.** The user-facing page lives in the `cloudflare-docs` repo, not here.
   Who owns that change, and must it land before or with the runtime change?
2. **Does the `"keys"` yield want `js.str()` or an explicit `jsg::JsString`?** Mechanical;
   `js.str(pair.key)` returns a `JsString` which converts implicitly to `JsValue`. Resolves at
   compile time — listed only so the implementer does not stall on it.

## 10. TODO list

Each step compiles, passes tests, and is revertible on its own.

- [ ] **1. API layer.** Add `Projection`, `ListState`, `parseProjection()`, the `projection`
      field, and the `jsg::JsArray` → `jsg::JsValue` widening. Parse the projection before the
      options are moved and before the empty-range early return. Add the trace tag using a
      `kj::ConstString` literal. Verify the `Next` JSG_STRUCT compiles with
      `Optional<jsg::JsValue>`.
- [ ] **2. Tests.** Extend `sync-kv-test.js` with keys/values/all cases, invalid-projection
      `TypeError` cases (including the empty-range ordering case), and cursor invalidation under
      a non-default projection. Append the matching spans to
      `sync-kv-instrumentation-test.js` in the same commit, including one span asserting the new
      `cloudflare.durable_object.kv.query.projection` tag. Run all three variants of
      `//src/workerd/api/tests:sync-kv-test@`.
- [ ] **3. Types.** Add the `JSG_TS_OVERRIDE` overloads and the `JSG_STRUCT_TS_OVERRIDE` field
      override, run `just generate-types`, commit the snapshot, and add a `test/types/` case
      asserting each projection's element type.
- [ ] **4. Docs.** Update the `storage.kv.list()` reference in `cloudflare-docs`, stating that
      `"values"` is ergonomic rather than faster and that the async `storage.list()` has no
      equivalent.
- [ ] **5. Storage layer.** Add `SqliteKv::listKeys()`, `ListCursor::nextKey()`, the
      `ListStatements` grouping with lazy key-only initialization, and the
      `SELECT *` → `SELECT key, value` cleanup. Extend `//src/workerd/util:sqlite-kv-test@`,
      including a case asserting `next()` fails on a `listKeys()` cursor. Switch the
      `Projection::KEYS` path in `listNext` to `nextKey()`. Land with the §7.7 benchmark sweep
      attached.

## Appendix A. Alternatives for carrying three element types

`jsg::IteratorBase` admits exactly one element type per iterator class (§5.2). Four ways to
accommodate three yield shapes were considered. The chosen design is the first.

### A.1 Widen the element type to `jsg::JsValue` (chosen)

One iterator class, one `next()`, element type `jsg::JsValue`. `"keys"` yields a bare string, so
`[...kv.list({projection: "keys"})]` is `["bar", "foo"]`.

Cost: the compiler no longer enforces that the yielded value is an array. That guarantee never
expressed the pair *shape*, only "is an array", so little is lost; `Projection` is stored in
`ListState` to keep the intent explicit at the yield site.

### A.2 Keep `jsg::JsArray`, yield one-element arrays

`"keys"` would yield `["foo"]` and `"values"` would yield `[value]`. Compiles, works, and is the
smallest diff — identical in size to A.1, since both change one macro argument. Rejected on five
grounds:

- It retains the per-row `v8::Array` allocation that the `"keys"` projection exists to eliminate.
  GC pressure scales with the number of rows listed, which is precisely the axis the feature is
  meant to improve.
- The static guarantee it preserves is hollow. `JsArray` asserts "is an array", not "is a
  `[key, value]` pair". Once arity varies by projection, the invariant that mattered has already
  moved out of the type system and into a runtime convention.
- It creates a silent-corruption path. `new Map(kv.list())` works today because the iterator
  yields two-element arrays; `new Map(kv.list({projection: "keys"}))` would produce
  `Map { key => undefined }` with no error.
- No other JS keys-iterator behaves that way. `Map.prototype.keys()`, `Object.keys()`,
  `URLSearchParams.keys()`, `Headers.keys()` and `FormData.keys()` all yield bare strings, so
  every call site would destructure `for (const [key] of ...)` permanently.
- The TypeScript surface gets worse, not simpler: `Iterable<[string]>` rather than
  `Iterable<string>`.

The yielded shape is observable behaviour, so this could not be corrected later without a
compatibility flag. Widening the C++ element type, by contrast, is invisible to users and does not
even alter the generated type snapshot (§5.4).

### A.3 Three iterator classes with a `kj::OneOf` return

`EntryIterator` / `KeyIterator` / `ValueIterator`, each with its exact element type, with `list()`
returning a `kj::OneOf` of three refs. This is the `URLSearchParams` shape
(`api/url-standard.h:60-71`), but there each of the three is reached by its own *method*, so no
union is involved.

Rejected: six new entries in `EW_SYNC_KV_ISOLATE_TYPES` (three classes plus three `::Next`
structs), three next functions, and a union return — in exchange for precision only on the keys
iterator, since a deserialized value is `jsg::JsValue` under any design.

### A.4 Template the iterator on `Projection`

`JSG_ITERATOR_TYPE` emits a concrete class, so this requires three instantiations and three
isolate-type registrations. Equivalent to A.3 with additional template machinery. Rejected.

A fifth shape — `Type = kj::OneOf<JsString, JsValue, JsArray>` — carries the same imprecision as
A.1 with more wrapping code, and was rejected without further consideration.
