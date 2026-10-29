# WPT modules workerd could run in the future

These results were measured on 2026-10-23 against WPT `2f17dec1f36194d1c236120fd38b3556be466072`, the
revision workerd vendors, and workerd main `7caf6dee5`.

Pass rates count only subtests that ran. A file that throws before it registers any subtests adds
nothing to either count, so suites with many such files look better than they are.

## Top-level directories workerd doesn't run yet

| Directory | Runnable (`.any` / other `.js`) | Measured pass rate | Harness change | Recommendation |
|---|---|---|---|---|
| **mimesniff/mime-types** | 1 any (+1 window) | 1285/1898 = 68%; Request/Response half **933/942 = 99%** | none | **add**. About 1,900 subtests come from one file. All the failures are in the Blob/File `type` normalization half, which browsers also fail heavily, so record them as expected failures, not bugs. Disable `charset-parameter.window.js`, which needs iframes. |
| **FileAPI** | 36 / 4 helpers (+5 window, 4 worker) | 375/586 = 64%; **Blob + File 327/388 = 84%** | none, but the 4 send-file-formdata files need `start_server` (41/46 with it) or must be disabled | **add**. See the FileAPI section below. |
| **user-timing** | 21 | 97/117 = **83%** (69% without the `before` hook) | trivial: clear marks and measures before each file (`before: () => { performance.clearMarks(); performance.clearMeasures(); }`), and add `assert_approx_equals` | **add** |
| **hr-time** | 3 (+1 window) | 33/37 = **89%** | trivial: `assert_approx_equals` | **add**. Small, but it reaches the hr-time idlharness. Disable `window-worker-timeOrigin.window.js`. |
| **console** | 7 + helper.js | 41/51 = **80%**; idlharness 28/28 | none | **add**. Only `console-label-conversion` fails. |
| webmessaging | 24 (+5 window, 4 helpers) | 24/62 = 39%; the MessageChannel-only subset is about 55–60% | none | maybe. BroadcastChannel is missing (18 files error out) and MessagePort transfer lists aren't supported. |
| workers | 31 (+160 helper js) | 42/177 = 24% | none | skip. These test the Worker and SharedWorker global scope; the passes are mostly trivial and there's a lot of noise. |
| storage | 8 | 34/92 = 37% | none | skip. There's no `navigator.storage` or StorageManager. |
| infrastructure | 8 (+24 test262 helpers) | 7/21 | none | skip. These test WPT itself. |
| dom (only `idlharness.any.js`, outside events/abort) | 1 | 180/764 = 24% | intrusive | skip. The harness sets `globalThis.Window`, so idlharness applies Window exposure and expects Node, Document, etc. |
| web-locks | 15 | 17/121 = 14% | none | skip. There's no `navigator.locks`. |
| cookiestore | 28 | 13/214 = 6% | none | skip. Not implemented. |
| service-workers/cache-storage | 13 | 1/20 = 5% | none | skip. CacheStorage `delete`/`keys`/`has` are unimplemented, so most files abort at setup. |
| scheduler | 21 | 1/28 = 4% | none | skip. There's no `scheduler.postTask` or `TaskController`. |
| js (builtins/weakrefs, etc.) | 9 `.optional` + 4 other | 0/32 | `step_wait` is missing from the harness | skip. `FinalizationRegistry.cleanupSome` isn't implemented, and the rest need iframes. |

These have no runnable tests at all, or test APIs workerd doesn't have: webstorage, IndexedDB, xhr,
webtransport, webcodecs, webnn, webusb, encrypted-media, permissions, notifications, push and
background-*. resource-timing, server-timing and timing-entrytypes-registry have only 1–6 files each,
and workerd doesn't produce those entries anyway.

### Notes on the "add" directories

**FileAPI.** Gaps: FileReader (49 ReferenceErrors, plus 26 idlharness failures),
`URL.createObjectURL` (15), `Blob.textStream` (7), the `endings` option (not implemented, 5), FileList,
and transfer lists. Real bugs:
- `unicode.any.js`: Blob doesn't replace unpaired surrogates. This is a USVString conversion bug.
- In Blob-constructor, 14 cases throw `Error` where `TypeError` is expected.
- In send-file-formdata, CR/LF in filenames gives "Unexpected multipart-shaped form data" (5 subtests).

**user-timing.** Performance entries leak between files because every file shares one isolate. The
remaining gaps:
- `measure()` doesn't throw SyntaxError for a missing mark.
- `detail` isn't structured-cloned: non-object values are rejected, and a Symbol doesn't throw.
- PerformanceMark and PerformanceMeasure prototypes don't chain to PerformanceEntry.
- A negative startTime is accepted.
- `supportedEntryTypes` is missing.

**hr-time.** Performance doesn't inherit from EventTarget, and `performance` lacks a getter on the
global.

**console.** count, countReset, time, timeLog and timeEnd don't call `toString()` on object labels, and
don't rethrow when it throws.

### Sketches of the targets

Every subdirectory already exists as a `@wpt//:<path>@module` target.

```
wpt_test(name = "FileAPI", size = "large", start_server = True,
    config = "FileAPI-test.ts", wpt_directory = "@wpt//:FileAPI@module")
wpt_test(name = "user-timing", config = "user-timing-test.ts", wpt_directory = "@wpt//:user-timing@module")
wpt_test(name = "hr-time", config = "hr-time-test.ts", wpt_directory = "@wpt//:hr-time@module")
wpt_test(name = "console", config = "console-test.ts", wpt_directory = "@wpt//:console@module")
wpt_test(name = "mimesniff/mime-types", config = "mimesniff/mime-types-test.ts",
    wpt_directory = "@wpt//:mimesniff/mime-types@module")
```

## html/webappapis

The vendored tarball has no `html/` or `css/`. workerd-tools PR
[#17](https://github.com/cloudflare/workerd-tools/pull/17) keeps `html/webappapis`, which adds about
108 KB gzipped. Once it lands, workerd needs a `freeze_version` and sha bump in
`build/deps/shared_deps.jsonc` and `build/deps/gen/shared_deps.MODULE.bazel`. The directory listing
(`build/wpt_get_directories.bzl`) is a glob, so it picks new directories up automatically.

These were measured through `--override_repository=+http+wpt=<copy with html added>`, using a scratch
harness that adds `generate_tests()`, maps absolute `/html/webappapis/<module>/x.js` META paths,
records each subtest's completion phase, and removes the `addEventListener` stub.

| Subdir | Runnable (.any) | Non-runnable (win/wkr + html) | Subtests passed/total | Files erroring before subtests | What's tested | workerd support | Harness change | Recommendation |
|---|---|---|---|---|---|---|---|---|
| **structured-clone** | 4 (1) | 0 + 2 | **117/137** | 0 | `structuredClone()` battery | yes | trivial: map absolute paths (or use `extra_wpt_directories`); omit the 3 battery helper files | **add**. The biggest real-bug yield. |
| **atob** | 1 (1) | 0 + 0 | **285/286** | 0 | atob/btoa | yes | trivial: add `generate_tests` and `fetch_json`, and bind `/fetch/data-urls/resources/base64.json` (already vendored) | **add** |
| **timers** | 9 (9) | 0 + 7 | 7/8, 1 incomplete | 0 (4 files register 0 subtests) | setTimeout/setInterval | yes; string handlers no | small: `setup({single_test})` and `done()` | **add**. Until single_test works, 4 files pass vacuously. |
| **microtask-queuing** | 3 (2) | 1 + 1 | 6/8; `.any` 6/6 | 0 | queueMicrotask, error rethrow | yes | remove the addEventListener stub | **add**. Omit the 2 MutationObserver subtests. |
| scripting | 22 (4) | 10 + 107 | 34/69, 5 incomplete | 11 | reportError, Atomics agent rules, unhandledrejection | partial | remove the stub; omit the support helpers and window/worker files; add `//# sourceURL` so `ErrorEvent.filename` isn't `eval` | maybe |
| system-state-and-capabilities | 4 (1) | 3 + 22 | 11/19; navigator.any 7/12 | 1 | navigator | partial | none | maybe (navigator.any only) |
| the-windoworworkerglobalscope-mixin | 2 (0) | 0 + 1 | 0/0 | 2 | self.origin in workers | no Worker | intrusive | skip |
| dynamic-markup-insertion | 51 (0) | 38 + 141 | 0/137 | 20 | document.write/open | no | n/a | skip |
| animation-frames, events, update-rendering, user-prompts | 0 | html only | – | – | requestAnimationFrame, DOM event handlers, alert/print | no | n/a | skip |

**Also worth adding:** `html/infrastructure/safe-passing-of-structured-data` scored 124/342. Its
`messagechannel.any.js` alone is 111/137, which makes it the MessageChannel version of the clone
battery. It needs `extra_wpt_directories` pointing at `webappapis/structured-clone`, and it would need
`html/infrastructure` vendored too; PR #17 only adds `webappapis`.

## workerd bugs and gaps these tests reveal

1. `MessagePort.postMessage` rejects any transfer list, even a plain ArrayBuffer
   (`api/messagechannel.c++:125`: "Transfer list is not supported").
2. `structuredClone` can't transfer a MessagePort. It throws TypeError "Object is not transferable"
   (`jsg/ser.c++:398`) instead of a DataCloneError.
3. `structuredClone(new Response())` succeeds. RPC-serializable types (`JSG_SERIALIZABLE`
   REQUEST/RESPONSE) leak into structuredClone, where the spec requires a DataCloneError.
4. Non-transferable, detached and out-of-bounds TypedArray/DataView transfers throw the wrong type:
   TypeError and similar, not a DataCloneError DOMException.
5. File isn't serializable, although Blob is.
6. `enhanced_error_serialization` (on by default since 2026-04-21) clones custom own properties and gives
   an empty Error an own `message`. This is a deliberate deviation from the spec, so its 8 subtests are
   expected failures.
7. `Atomics.waitAsync(ta, 0, 0, 10)` never resolves.
8. MessagePort messages run before unhandledrejection reporting and before `setTimeout(0)`, i.e. they
   behave like microtasks rather than tasks.
9. unhandledrejection fires out of rejection order for a batch of promises, and sometimes not at all.
10. With `nodejs_compat`, `setTimeout` returns Timeout objects instead of numeric handles. String
    handlers aren't supported.
11. Missing members: `isSecureContext`, `crossOriginIsolated`, `navigator.appCodeName`/`appName`/
    `appVersion`/`product`. SharedArrayBuffer exists without cross-origin isolation, and DOMException
    `.stack` isn't preserved through clone.
12. fetch() doesn't decode zstd Content-Encoding. `api/system-streams.c++` handles only gzip and br, even
    though `api/compression.c++` has zstd. (From `fetch/misc`.)
13. A 1xx final response rejects with `Error` instead of `TypeError` (fetch/security/1xx-response).
14. Async `WebAssembly.compile`/`instantiate` of bytes never settles when nothing else is pending. V8
    finishes the compile on a foreground task posted from a background thread, and nothing wakes the KJ
    event loop to run it. It's mostly reachable through `unsafeEval`. (From wasm/jsapi.)

## WPT harness bugs to fix before adding more suites

These affect results across suites, so fix them first.

1. `src/wpt/harness/globals.ts` replaces `globalThis.addEventListener` with a no-op. Its comment says
   workerd lacks a global addEventListener, but module workers have one. The stub hides real global
   events such as `error` and `unhandledrejection`. Removing it turned microtask-exceptions from a hang
   into a pass. Its effect on the other suites hasn't been checked.
2. `WPTSubtestResult` reports subtests that hung or never finished as PASS, because it only checks
   `error`. In scripting, 43/69 were reported as passing when only 22 actually completed, so existing
   suites may be overstated.
3. The Starlark `relative_path()` in `build/wpt_test.bzl` is mis-ported: its `elif from_len > length`
   sits at the wrong nesting level. Deep target names such as
   `html/infrastructure/safe-passing-of-structured-data` then get broken embed paths.
4. `createBuffer()` ignores `maxByteLength`, so "Growable SharedArrayBuffer" fails falsely.
5. The harness lacks `generate_tests`, `fetch_json`, `setup({single_test})`, `assert_approx_equals` and
   `step_wait`.
6. `promise_rejects_dom` uses `.then(...).catch(...)`, so a promise that fulfills is reported with a
   misleading "not a DOMException… ERR_ASSERTION" message.
7. Discovery picks up `support/*.js` and other helper files, so configs need `omittedTests` entries for
   them.
8. Every wptserve run leaks about 50 Python multiprocessing semaphores (`sem.mp-*`) into `/dev/shm`,
   which is only 63 MB in the devcontainer. After roughly 320 runs `/dev/shm` fills up and every suite
   with `start_server` fails with "No space left on device". The sidecar shutdown probably kills
   wptserve without a graceful exit.

## Scratch state from these investigations

- `/workspaces/workerd-wpt-survey`: detached at main with 15 `survey/*` targets. Uncommitted.
- `/workspaces/workerd-wpt-html`: the scratch harness changes (marked SCRATCH) and `src/wpt/html/**`
  configs. Uncommitted.
- `/workspaces/wpt-override-html`: the WPT override repo, with html/ added.
- `/workspaces/wpt-html-webappapis`: a sparse upstream WPT clone at `2f17dec1f`.
- `/tmp/wptsurvey/` and `/tmp/wpt-html-scripts/`: the parsers and probes. Anything in `/tmp` may not
  survive a restart.
