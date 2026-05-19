// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-335:
// Heap buffer overflow (OOB write) in URLPattern regex_search via
// monkey-patched RegExp.prototype.exec.
//
// URLPatternRegexEngine::regex_search allocated a std::vector sized by the
// initial matches.size(), then re-read matches.size() in the loop condition
// while calling matches.get() which can fire user-defined getters. A
// monkey-patched RegExp.prototype.exec could return an array that grows
// mid-iteration, causing OOB writes past the vector's backing store.
//
// The fix snapshots the array length once before iterating and uses
// reserve+emplace_back instead of operator[]. JsRegExp::operator() also
// now rejects non-Array return values from exec.

import { strictEqual, ok } from 'node:assert';

export const urlpatternRegexSearchOobRegression = {
  async test() {
    const realExec = RegExp.prototype.exec;

    // Test 1: Monkey-patched exec that tries to grow the result array
    // mid-iteration should not cause a crash or OOB write. After the fix,
    // the snapshotted length prevents the loop from reading past the
    // allocated vector.
    let armed = false;
    RegExp.prototype.exec = function (s) {
      if (!armed) return realExec.call(this, s);

      // Return an array with initial length 3 (match + 2 groups).
      // The getter on index 2 tries to grow the array to 64 elements.
      const arr = ['match', 'AAAAAAAA'];
      Object.defineProperty(arr, 2, {
        enumerable: true,
        configurable: true,
        get() {
          // Attempt to grow the array past the pre-allocated vector size.
          for (let j = 3; j < 64; j++) arr[j] = 'B'.repeat(40);
          return 'CCCCCCCC';
        },
      });
      arr.length = 3;
      return arr;
    };

    try {
      const p = new URLPattern({ pathname: '/(x)' });
      armed = true;

      // After the fix, this should either succeed safely (returning results
      // based on the snapshotted length) or throw a TypeError — but must NOT
      // crash the process with a heap-buffer-overflow.
      try {
        const result = p.exec({ pathname: '/x' });
        // If it succeeds, the result should have the pathname group.
        if (result !== null) {
          strictEqual(typeof result.pathname, 'object');
        }
      } catch (_e) {
        // A TypeError from the hardened JsRegExp::operator() rejecting
        // non-standard exec results is acceptable.
      }
      // The key assertion: we reached this point without a process crash.
      ok(true, 'Process did not crash from array-growing getter attack');
    } finally {
      armed = false;
      RegExp.prototype.exec = realExec;
    }

    // Test 2: Monkey-patched exec returning a non-array should not crash.
    RegExp.prototype.exec = function (s) {
      if (!armed) return realExec.call(this, s);
      // Return a plain object instead of an array.
      return { 0: 'match', 1: 'group', length: 2 };
    };

    try {
      const p2 = new URLPattern({ pathname: '/(y)' });
      armed = true;
      // Should not crash — the hardened code rejects non-Array results.
      const result2 = p2.exec({ pathname: '/y' });
      // After the fix, non-array results are treated as no-match (null).
      strictEqual(result2, null);
    } finally {
      armed = false;
      RegExp.prototype.exec = realExec;
    }

    // Test 3: Monkey-patched exec returning empty array (length 0) should
    // not cause integer underflow (size_t wrapping to ~4G).
    RegExp.prototype.exec = function (s) {
      if (!armed) return realExec.call(this, s);
      return [];
    };

    try {
      const p3 = new URLPattern({ pathname: '/(z)' });
      armed = true;
      // Should not crash with OOM from 4G-element vector allocation.
      // The empty array is treated as a match with zero groups by ada-url,
      // so exec() returns a result object (not null). The key assertion is
      // that we don't crash from the integer underflow.
      const _result3 = p3.exec({ pathname: '/z' });
      ok(true, 'Process did not crash from empty-array underflow attack');
    } finally {
      armed = false;
      RegExp.prototype.exec = realExec;
    }
  },
};
