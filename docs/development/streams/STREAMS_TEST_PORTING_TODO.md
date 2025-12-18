# Streams API Test Porting TODO

This document tracks the progress of porting streams API tests from the internal project's `.ew-test` format to workerd's `.wd-test` format.

## Overview

The goal is to migrate streams-related tests from the internal repo to workerd where feasible. Some tests require internal-repo specific features (like `%CF_CONTROL_HEADER`, `@SUBREQUEST`, pipeline configurations, or internal debug APIs) and may not be portable.

## Completed Porting Tasks

### compression-streams-test.js (FULLY PORTED)
**Source**: `{internal}/api-tests/streams/compression.ew-test`

All 12 tests ported:
- [x] compression-gzip
- [x] compression-deflate
- [x] compression-deflate-raw
- [x] compression-pending-read
- [x] compression-cancel-abort
- [x] decompression-error
- [x] decompression-error-iteration
- [x] piped-decompression-bad-data
- [x] transformstream-through-compression
- [x] request-body-pipethrough
- [x] transform-roundtrip
- [x] compression-pipeline

**ew-test can be deleted**: YES

---

### encoding-streams-test.js (FULLY PORTED)
**Source**: `{internal}/api-tests/streams/encoding.ew-test`

All 5 tests ported:
- [x] textencoder-stream-roundtrip
- [x] textencoder-stream-types
- [x] textdecoder-stream-big5
- [x] textencoder-stream-encoding
- [x] textdecoder-stream-encodings

**ew-test can be deleted**: YES

---

### transform-streams-test.js (MOSTLY PORTED)
**Source**: `{internal}/api-tests/streams/transform.ew-test`

Ported tests (12):
- [x] default-identity-transform
- [x] simple-transform
- [x] delay-transform
- [x] different-types-transform
- [x] sync-error-during-start
- [x] async-error-during-start
- [x] sync-error-during-transform
- [x] async-error-during-transform
- [x] sync-error-during-flush
- [x] async-error-during-flush
- [x] write-backpressure
- [x] transform-stream-cancel

Remaining tests (2):
- [ ] **transform-roundtrip**: Requires `request.body` with gzip Content-Encoding
  - *Approach*: Create a service binding that returns a gzip-compressed body, then pipe through decompression/transform/identity
- [ ] **transform-crash**: Regression test iterating over `globalThis` properties (low priority)

---

### pipe-streams-test.js (MOSTLY PORTED)
**Source**: `{internal}/api-tests/streams/pipe.ew-test`

Ported tests (32):
- [x] pipethrough-js-to-internal: Ported as pipeThroughJsToInternal
- [x] pipethrough-js-to-internal-errored-source: Ported as pipeThroughJsToInternalErroredSource
- [x] pipeto-js-to-internal-errored-source: Ported as pipeToJsToInternalErroredSource
- [x] pipethrough-js-to-internal-errored-source-prevent: Ported as pipeThroughJsToInternalErroredSourcePreventAbort
- [x] pipeto-js-to-internal-errored-source-prevent: Ported as pipeToJsToInternalErroredSourcePreventAbort
- [x] pipethrough-js-to-internal-errored-dest: Ported as pipeThroughJsToInternalErroredDest
- [x] pipeto-js-to-internal-errored-dest: Ported as pipeToJsToInternalErroredDest
- [x] pipethrough-js-to-internal-closes: Ported as pipeThroughJsToInternalCloses
- [x] pipethrough-js-to-internal-prevent-close: Ported as pipeThroughJsToInternalPreventClose
- [x] simple-pipethrough-js-byob-to-internal: Ported as pipeThroughJsByobToInternal
- [x] simple-pipeto-js-byob-to-internal: Ported as pipeToJsByobToInternal
- [x] pipethrough-internal-to-js: Ported as pipeThroughInternalToJs
- [x] pipeto-internal-to-js: Ported as pipeToInternalToJs
- [x] simple-pipeto-internal-to-js: Ported as pipeToInternalToJsSimple
- [x] pipeto-internal-to-js-error: Ported as pipeToInternalToJsError
- [x] pipeto-internal-to-js-error-prevent: Ported as pipeToInternalToJsErrorPrevent
- [x] pipeto-internal-to-js-close: Ported as pipeToInternalToJsClose
- [x] pipeto-internal-to-js-close-prevent: Ported as pipeToInternalToJsClosePrevent
- [x] pipethrough-js-to-js: Ported as pipeThroughJsToJs
- [x] simple-pipeto-js-to-js: Ported as pipeToJsToJsSimple
- [x] simple-pipeto-js-to-js-error-readable: Ported as pipeToJsToJsErrorReadable
- [x] simple-pipeto-js-to-js-error-readable-prevent: Ported as pipeToJsToJsErrorReadablePrevent
- [x] simple-pipeto-js-to-js-error-writable: Ported as pipeToJsToJsErrorWritable
- [x] simple-pipeto-js-to-js-error-writable-prevent: Ported as pipeToJsToJsErrorWritablePrevent
- [x] simple-pipeto-js-to-js-close-readable: Ported as pipeToJsToJsCloseReadable
- [x] simple-pipeto-js-to-js-close-readable-prevent: Ported as pipeToJsToJsCloseReadablePrevent
- [x] simple-pipeto-js-to-js-tee: Ported as pipeToJsToJsTee
- [x] cancel-pipeto-js-to-js-already: Ported as pipeToJsToJsCancelAlready
- [x] cancel-pipeto-js-to-native-already: Ported as pipeToJsToNativeCancelAlready
- [x] cancel-pipeto-js-to-js: Ported as pipeToJsToJsCancel
- [x] cancel-pipeto-js-to-native: Ported as pipeToJsToNativeCancel

NOT PORTED (require internal-only features or cause unhandled rejection errors):
- [ ] **simple-pipeto-js-to-internal**: Causes unhandled promise rejection in wd-test
      environment before the error can be caught (pipe rejects while consuming readable)
- [ ] **pipethrough-js-to-internal-errored-dest-prevent**: Causes unhandled promise rejection
- [ ] **pipeto-js-to-internal-errored-dest-prevent**: Causes unhandled promise rejection
- [ ] **never-ending-pipethrough**: Tests CPU limit enforcement, requires internal harness
- [ ] **pipeto-double-close**: Requires request body piping which needs HTTP request/response cycle
- [ ] **pipeto-internal-to-js-error-readable**: Tests internal readable error propagation,
      requires complex setup with IdentityTransformStream's internal behavior
- [ ] **pipeto-internal-to-js-error-readable-prevent**: Same as above with preventCancel=true

---

## Partially Ported

### streams.ew-test (PARTIALLY PORTED)
**Source**: `{internal}/api-tests/streams.ew-test`

Ported tests (11) - added to streams-test.js:
- [x] testReadableStream cancel tests: Ported as cancelStreamRejectsBodyConsume, cancelReaderResolvesClosedPromise
- [x] getReader mode validation: Ported as getReaderBadModeThrows
- [x] stream locking: Ported as streamLockedAfterGetReader
- [x] BYOB reader constraints: Ported as byobReaderConstraints
- [x] testReadableStreamCancelErrors: Ported as cancelErrorTypePropagation
- [x] testTransformStream write/read ordering: Ported as identityTransformWriteBeforeRead, identityTransformReadBeforeWrite
- [x] closed promise behavior: Ported as closedPromiseUnderLockRelease, closedPromiseUnderWriterAbort
- [x] testFixedLengthStream: Ported as fixedLengthStreamPreconditions

NOT PORTED (require HTTP harness or internal features):
- [ ] cancel-subrequest: Requires subrequests with hung response
- [ ] identity-transform-stream scripts: Require HTTP request/response cycle
- [ ] identity-transform-stream-simple-esi: Requires subrequests
- [ ] testReadableStreamPipeTo: Most tests require fetch/subrequests
- [ ] testReadableStreamTee: Requires fetch/subrequests and memory limits
- [ ] readable-stream-pipe-to: Requires subrequests
- [ ] transform-stream-no-hang: Tests request hang prevention
- [ ] tee-*-no-hang scripts: Require HTTP request cycle
- [ ] fixed-length-stream scripts: Require subrequests
- [ ] And many more that require the HTTP harness

---

## Not Yet Started - Porting Tasks

---

### streams-js.ew-test (LARGE)
**Source**: `{internal}/api-tests/streams-js.ew-test`

JavaScript-backed streams tests. Need to review for portable tests.

---

### streams-nonstandard.ew-test
**Source**: `{internal}/api-tests/streams-nonstandard.ew-test`

Non-standard extension tests:
- [ ] **readable-stream-minbytes-read**: Tests `readAtLeast()` with default and BYOB readers
  - *Approach*: Can be ported using service binding to provide chunked input

---

### respond.ew-test
**Source**: `{internal}/api-tests/streams/respond.ew-test`

Tests for using JS ReadableStreams as Response bodies:
- [ ] **js-source**: Basic JS ReadableStream as response
- [ ] **js-source-2**: JS ReadableStream with async pull
- [ ] **js-byte-source**: BYOB ReadableStream as response
- [ ] **js-byte-source-2**: BYOB with multiple chunks
- [ ] **js-tee-source**: Teed JS ReadableStream as response
- [ ] Various other response body patterns

---

### r2-patterns.ew-test
**Source**: `{internal}/api-tests/streams/r2-patterns.ew-test`

R2-style streaming patterns:
- [ ] **closed-byob-tee-on-start**: BYOB tee closed on start
- [ ] **request-clone-byob**: readAtLeast with cloned request
- [ ] **pending-byob-readatleast**: Automatic atLeast handling
- [ ] **pending-byob-readatleast-2**: Manual atLeast handling

---

### streams-iocontext.ew-test
**Source**: `{internal}/api-tests/streams/streams-iocontext.ew-test`

Tests for streams created at global scope and used across requests:
- [ ] **global-scope-readablestream**: Basic global scope usage
- [ ] **global-scope-readablestream-2** through **8**: Various patterns

*Note*: These tests may require specific isolate behavior that wd-test doesn't support.

---

### streams-gc.ew-test (NOT PORTABLE)
**Source**: `{internal}/api-tests/streams/streams-gc.ew-test`

**Cannot be ported**: Requires `env.debug.makeGcDetectorPair()` which is an internal debug API not available in workerd.

---

## Implementation Guidance

This section here is primarily intended a a guide/helper for claude to help with the
automation of porting the tests. It provides additional context on how to implement
the tests in workerd's test framework.

Test that cannot be ported easily should be noted in the relevant sections above and
in the wd-test test js files themselves with sufficient detail to explain why they
cannot be ported. When a test cannot be sufficiently ported, it should be marked as TODO
with an explanation rather than being deleted.

### Adding a New Test

1. Create or extend a test file in `src/workerd/api/tests/`
2. Export tests as named exports with `async test()` methods:
   ```javascript
   export const myTestName = {
     async test() {
       // Test code using node:assert
     },
   };
   ```
3. Update or create the `.wd-test` config file
4. Add the test to `BUILD.bazel`:
   ```python
   wd_test(
       src = "my-test.wd-test",
       args = ["--experimental"],
       data = ["my-test.js"],
   )
   ```

### Handling Tests That Need Subrequests

Use self-referencing service bindings:

```javascript
// In the test file
export const mySubrequestTest = {
  async test(_ctrl, env) {
    const response = await env.SERVICE.fetch('http://test/endpoint');
    // ... test the response
  },
};

// Default handler for subrequests
export default {
  async fetch(request) {
    if (request.url.includes('/endpoint')) {
      return new Response('expected data');
    }
    return new Response('Not found', { status: 404 });
  },
};
```

### Handling Async Errors

When testing async operations that should reject:

```javascript
import { rejects } from 'node:assert';

// Use rejects() for promise rejections
await rejects(somePromise, { message: 'expected error' });

// Use Promise.allSettled() when multiple promises may reject
const results = await Promise.allSettled([promise1, promise2]);
strictEqual(results[0].status, 'rejected');
strictEqual(results[0].reason.message, 'expected');
```

### Documenting Porting Status

After porting tests, add a status comment at the end of each test file and update this document.
```javascript
// =============================================================================
// Porting status from {internal}/api-tests/streams/foo.ew-test
//
// PORTED (can be deleted from ew-test):
// - test-name-1: Ported as testName1
// - test-name-2: Ported as testName2
//
// TODO: The following tests remain to be ported:
// - test-name-3: Description of why it's not yet ported
// =============================================================================
```

---

## Checklist Summary

| Source File | Status | Ported | Remaining | Can Delete ew-test |
|-------------|--------|--------|-----------|-------------------|
| compression.ew-test | Complete | 12 | 0 | Yes |
| encoding.ew-test | Complete | 5 | 0 | Yes |
| transform.ew-test | Mostly done | 12 | 2 | No |
| pipe.ew-test | Mostly done | 32 | 7 | No (7 tests can't be ported) |
| streams.ew-test | Partial | 11 | ~30 | No (many require HTTP harness) |
| streams-js.ew-test | Not started | 0 | ? | No |
| streams-nonstandard.ew-test | Not started | 0 | 1 | No |
| respond.ew-test | Not started | 0 | ~10 | No |
| r2-patterns.ew-test | Not started | 0 | ~4 | No |
| streams-iocontext.ew-test | Not started | 0 | ~8 | No |
| streams-gc.ew-test | Not portable | 0 | 1 | No - requires internal API |
