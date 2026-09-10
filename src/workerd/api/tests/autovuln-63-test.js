// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, rejects } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-63.
//
// A stream algorithm stored in an `Algorithms` struct can be freed while it is
// still executing. `maybeRunAlgorithm` (streams/standard.c++) invokes the
// algorithm through a bare reference into the `kj::Maybe` that owns it, so any
// re-entrant JS that reaches `algorithms.clear()` drops what is often the last
// reference to the `jsg::Function`.
//
// This only bites for algorithms backed by a *native* C++ lambda. Those are held
// as `Ref<WrappableFunctionImpl>`, which is destroyed immediately once the last
// reference goes away, taking the lambda's captured state with it. Algorithms
// backed by a JS function are unaffected: `jsg::Function::operator()` reads them
// out as `v8::Local` handles before the call, and those stay rooted in the
// enclosing HandleScope.
//
// The fix keeps a strong reference to the function for the duration of the call,
// in `jsg::Function::operator()` (jsg/function.h).

// The actual vulnerability.
//
// TextEncoderStream's transform algorithm is a native lambda that captures a
// `kj::Rc<Holder>` and coerces its chunk with `chunk.toJsString(js)` — a raw V8
// ToString, so a user-supplied `toString()` runs in the middle of the algorithm.
// Cancelling the readable side from there reaches
// TransformStreamDefaultController::errorWritableAndUnblockWrite, which calls
// `algorithms.clear()`. Without the fix the transform lambda is freed there, and
// dereferencing `holder` immediately after the ToString returns is a
// use-after-free (heap-use-after-free in `kj::Rc<Holder>::operator->()`).
export const transformAlgorithmFreedViaCancelDuringToString = {
  async test() {
    const { writable, readable } = new TextEncoderStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();

    const readPromise = reader.read();

    let toStringCalled = false;

    const writePromise = rejects(
      writer.write({
        toString() {
          toStringCalled = true;
          reader.cancel(new Error('boom'));
          return 'hello after free';
        },
      }),
      {
        message: /The readable side/,
      }
    );

    await Promise.all([readPromise, writePromise]);

    ok(toStringCalled, 'toString should have been called');
  },
};

// Behavioural coverage only — this does NOT reproduce the vulnerability, and
// passes with or without the fix. The cancel algorithm here is a JS function, so
// it takes the HandleScope-rooted path described above.
//
// It is kept because calling `controller.error()` from inside an underlying
// source's `cancel()` is otherwise untested: the WPT suite only has the
// TransformStream analogue (transform-streams/cancel.any.js), which is an
// expected failure for the C++ streams implementation. The equivalent
// re-entrant-error cases for `pull` and for a writable's `write`/`abort` are
// already covered by streams-error-edge-cases-test.js, streams-js-test.js,
// standard-test.c++ and enabled WPT tests, so they are deliberately not
// duplicated here.
export const readableCancelAlgorithmErrorsController = {
  async test() {
    let cancelCalled = false;
    let ctrl;

    const rs = new ReadableStream({
      start(c) {
        ctrl = c;
      },
      cancel(_reason) {
        cancelCalled = true;
        // Re-entrant error during cancel clears algorithms.
        ctrl.error(new Error('cancel-error'));
      },
    });

    const reader = rs.getReader();
    await reader.cancel('test');

    ok(cancelCalled, 'cancel should have been called');
  },
};
