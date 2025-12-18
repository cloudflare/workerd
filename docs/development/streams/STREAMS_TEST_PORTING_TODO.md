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

### pipe-streams-test.js (PARTIALLY PORTED)
**Source**: `{internal}/api-tests/streams/pipe.ew-test`

Ported tests (6):
- [x] pipethrough-js-to-internal
- [x] pipethrough-js-to-internal-errored-source
- [x] pipeto-js-to-internal-errored-source
- [x] pipethrough-internal-to-js
- [x] pipeto-internal-to-js
- [x] pipethrough-js-to-js

Remaining tests - JS to Internal stream piping:
- [ ] **simple-pipeto-js-to-internal**: Tests pipeTo from JS ReadableStream to internal IdentityTransformStream writable when a non-byte chunk causes an error
  - *Note*: Similar to pipeThroughJsToInternal but uses pipeTo directly
- [ ] **pipethrough-js-to-internal-errored-source-prevent-abort**: Tests `preventAbort=true` keeps writable usable after source errors
- [ ] **pipeto-js-to-internal-errored-source-prevent-abort**: Same as above but with pipeTo

Remaining tests - Error propagation with errored destination:
- [ ] **pipethrough-js-to-internal-errored-dest**: Tests error propagation when destination writable is canceled
- [ ] **pipeto-js-to-internal-errored-dest**: Same with pipeTo
- [ ] **pipethrough-js-to-internal-errored-dest-prevent**: Tests `preventCancel=true` keeps source readable usable
- [ ] **pipeto-js-to-internal-errored-dest-prevent**: Same with pipeTo

Remaining tests - Close propagation:
- [ ] **pipethrough-js-to-internal-closes**: Tests closing readable closes writable when `preventClose=false`
- [ ] **pipethrough-js-to-internal-prevent-close**: Tests `preventClose=true` keeps writable open

Remaining tests - BYOB streams:
- [ ] **simple-pipethrough-js-byob-to-internal**: pipeThrough with BYOB ReadableStream
- [ ] **simple-pipeto-js-byob-to-internal**: pipeTo with BYOB ReadableStream

Remaining tests - Internal to JS:
- [ ] **simple-pipeto-internal-to-js**: Basic pipeTo from internal readable to JS writable
- [ ] **pipeto-internal-to-js-error**: Error in internal readable aborts JS writable
- [ ] **pipeto-internal-to-js-error-prevent**: `preventAbort=true` keeps JS writable usable
- [ ] **pipeto-internal-to-js-error-readable**: Error in JS writable cancels internal readable
- [ ] **pipeto-internal-to-js-error-readable-prevent**: `preventCancel=true` keeps internal readable usable
- [ ] **pipeto-internal-to-js-close**: Closing internal readable closes JS writable
- [ ] **pipeto-internal-to-js-close-prevent**: `preventClose=true` keeps JS writable open

Remaining tests - JS to JS:
- [ ] **simple-pipeto-js-to-js**: Basic JS-to-JS pipeTo
- [ ] **simple-pipeto-js-to-js-error-readable**: Error in JS readable aborts JS writable
- [ ] **simple-pipeto-js-to-js-error-readable-prevent**: `preventAbort=true`
- [ ] **simple-pipeto-js-to-js-error-writable**: Error in JS writable cancels JS readable
- [ ] **simple-pipeto-js-to-js-error-writable-prevent**: `preventCancel=true`
- [ ] **simple-pipeto-js-to-js-close-readable**: Closing readable closes writable
- [ ] **simple-pipeto-js-to-js-close-readable-prevent**: `preventClose=true`
- [ ] **simple-pipeto-js-to-js-tee**: pipeTo from a tee branch

Remaining tests - AbortSignal:
- [ ] **cancel-pipeto-js-to-js-already**: pipeTo with already-aborted signal
- [ ] **cancel-pipeto-js-to-native-already**: Same with native writable
- [ ] **cancel-pipeto-js-to-js**: pipeTo cancelable during operation
- [ ] **cancel-pipeto-js-to-native**: Same with native writable

Remaining tests - Edge cases:
- [ ] **never-ending-pipethrough**: Never-ending readable handled by CPU limits
- [ ] **pipeto-double-close**: Double close is handled correctly

---

## Not Yet Started - Porting Tasks

### streams.ew-test (LARGE - needs triage)
**Source**: `{internal}/api-tests/streams.ew-test`

This is a large file with many diverse tests. Tests to port:

**ReadableStream cancellation tests**:
- [ ] cancel-subrequest: Tests canceling a subrequest body mid-read

**IdentityTransformStream tests**:
- [ ] identity-transform-stream: Multiple sub-tests for basic IdentityTransformStream usage
- [ ] identity-transform-stream-simple-esi: Edge Side Includes pattern test

**W3C-style ReadableStream tests** (api-streams bundle):
- [ ] testReadableStream: Various cancel/read/lock tests
- [ ] testReadableStreamPipeTo: Comprehensive pipeTo tests with error propagation
- [ ] testReadableStreamTee: tee() tests including memory limits
- [ ] testReadableStreamCancelErrors: Cancel error type preservation
- [ ] testTransformStream: Identity transform tests
- [ ] testFixedLengthStream: Constructor validation

**ReadableStream.pipeTo() tests**:
- [ ] readable-stream-pipe-to: Concatenating multiple readables

**Hang prevention tests**:
- [ ] transform-stream-no-hang: TransformStream read without write
- [ ] tee-transform-stream-no-hang: Teed TransformStream ignoring one branch
- [ ] tee-readable-stream-no-hang: Teed ReadableStream ignoring one branch

**FixedLengthStream tests**:
- [ ] fixed-length-stream: Basic usage with fetch concatenation
- [ ] fixed-length-stream-incorrect: Truncation and overwrite handling

**Misc tests**:
- [ ] reader-doesnt-hold-state: Reader release doesn't corrupt gzip
- [ ] close-stream-cross-request: Cross-request stream access errors
- [ ] ts-doesnt-hang-response-text: Response.text() on TransformStream
- [ ] teed-ts-doesnt-hang-if-not-closed: Teed TS without close
- [ ] readable-stream-pipe-to-simple-cycle: Cycle detection
- [ ] transform-stream-write-error-causes-premature-finalization: Error propagation

**Memory limit tests**:
- [ ] system-stream-tee-errors: Tee buffer limit errors

**Constructor tests**:
- [ ] writablestream-writer-constructors
- [ ] reader-constructors-work

**BYOB tests**:
- [ ] byob-reader-detaches-buffer
- [ ] write-subarray

**Async iterator tests**:
- [ ] for-await-readablestream
- [ ] for-await-readablestream-values
- [ ] for-await-readablestream-early-return
- [ ] for-await-readablestream-early-throw
- [ ] for-await-readablestream-early-return-preventcancel
- [ ] for-await-readablestream-early-throw-preventcancel
- [ ] for-await-readablestream-gc

**Feature flag tests**:
- [ ] capture-sync-throws: captureThrowsAsRejections flag

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
| pipe.ew-test | Partial | 6 | ~30 | No |
| streams.ew-test | Not started | 0 | ~40 | No |
| streams-js.ew-test | Not started | 0 | ? | No |
| streams-nonstandard.ew-test | Not started | 0 | 1 | No |
| respond.ew-test | Not started | 0 | ~10 | No |
| r2-patterns.ew-test | Not started | 0 | ~4 | No |
| streams-iocontext.ew-test | Not started | 0 | ~8 | No |
| streams-gc.ew-test | Not portable | 0 | 1 | No - requires internal API |
