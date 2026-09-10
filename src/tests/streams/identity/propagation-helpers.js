// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Helpers for asserting how an abort/cancel reason surfaces on the far side
// of an identity stream. The two implementations deliberately diverge:
// - TypeScript: the reason stays in JavaScript, so the original instance
//   surfaces everywhere.
// - C++: the reason is tunneled through a kj::Exception, so what surfaces is
//   a re-created Error carrying the same message, never the original
//   instance. (Exception: a reason the writable side holds directly as a JS
//   value, such as writer.closed after abort — callers assert that case with
//   plain strictEqual.)

import { ok, strictEqual, notStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Awaits the promise, requires it to reject, and returns the rejection
// value. (node:assert's rejects() does not return the error.)
export async function captureRejection(promise) {
  let rejected = false;
  let err;
  await promise.then(
    () => {},
    (e) => {
      rejected = true;
      err = e;
    }
  );
  ok(rejected, 'Expected promise to reject, but it resolved');
  return err;
}

// Asserts that the promise rejects with the original reason instance
// (TypeScript) or with a re-created Error carrying the same message (C++).
export async function assertRejectsWithReason(promise, reason) {
  const err = await captureRejection(promise);
  if (usingTsImpl) {
    strictEqual(err, reason);
  } else {
    notStrictEqual(err, reason);
    ok(err instanceof Error);
    strictEqual(err.name, reason.name);
    strictEqual(err.message, reason.message);
  }
  return err;
}
