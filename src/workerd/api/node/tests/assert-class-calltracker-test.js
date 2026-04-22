// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import * as assertMod from 'node:assert';
import {
  Assert as AssertNamed,
  CallTracker as CallTrackerNamed,
} from 'node:assert';
import strictDefault from 'node:assert/strict';
import * as strictMod from 'node:assert/strict';
import {
  Assert as AssertStrictNamed,
  CallTracker as CallTrackerStrictNamed,
} from 'node:assert/strict';

export const assertAssertClassTest = {
  test() {
    // Available on the default export, the namespace import, and as a named
    // import.
    assert.strictEqual(typeof assert.Assert, 'function');
    assert.strictEqual(typeof assertMod.Assert, 'function');
    assert.strictEqual(typeof AssertNamed, 'function');
    assert.strictEqual(assert.Assert, assertMod.Assert);
    assert.strictEqual(assert.Assert, AssertNamed);

    // It is a class — has a prototype with a constructor pointing back at it.
    assert.strictEqual(typeof assert.Assert.prototype, 'object');
    assert.strictEqual(assert.Assert.prototype.constructor, assert.Assert);

    // Constructable with and without options.
    const a = new assert.Assert();
    assert.ok(a instanceof assert.Assert);
    const b = new assert.Assert({ message: 'hi', operator: '==' });
    assert.ok(b instanceof assert.Assert);
  },
};

export const assertCallTrackerClassTest = {
  test() {
    assert.strictEqual(typeof assert.CallTracker, 'function');
    assert.strictEqual(typeof assertMod.CallTracker, 'function');
    assert.strictEqual(typeof CallTrackerNamed, 'function');
    assert.strictEqual(assert.CallTracker, assertMod.CallTracker);
    assert.strictEqual(assert.CallTracker, CallTrackerNamed);

    assert.strictEqual(typeof assert.CallTracker.prototype, 'object');
    assert.strictEqual(
      assert.CallTracker.prototype.constructor,
      assert.CallTracker
    );

    const tracker = new assert.CallTracker();
    assert.ok(tracker instanceof assert.CallTracker);

    // .report() returns an array.
    const report = tracker.report();
    assert.ok(Array.isArray(report));
    assert.strictEqual(report.length, 0);

    // .verify() and .reset() are inert and do not throw.
    assert.strictEqual(tracker.verify(), undefined);
    assert.strictEqual(tracker.reset(), undefined);

    // .getCalls() returns an array.
    const calls = tracker.getCalls(() => {});
    assert.ok(Array.isArray(calls));

    // .calls() throws ERR_METHOD_NOT_IMPLEMENTED.
    assert.throws(() => tracker.calls(() => {}), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
    assert.throws(() => tracker.calls(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
  },
};

export const assertStrictAssertAndCallTrackerTest = {
  test() {
    // Available via the `strict` property of `assert`.
    assert.strictEqual(typeof assert.strict.Assert, 'function');
    assert.strictEqual(typeof assert.strict.CallTracker, 'function');
    assert.strictEqual(assert.strict.Assert, assert.Assert);
    assert.strictEqual(assert.strict.CallTracker, assert.CallTracker);

    // Available via the `node:assert/strict` module — both the default export
    // (which is the same `assert` object) and as named exports re-exported
    // through `export *`.
    assert.strictEqual(typeof strictDefault.Assert, 'function');
    assert.strictEqual(typeof strictDefault.CallTracker, 'function');
    assert.strictEqual(typeof strictMod.Assert, 'function');
    assert.strictEqual(typeof strictMod.CallTracker, 'function');
    assert.strictEqual(typeof AssertStrictNamed, 'function');
    assert.strictEqual(typeof CallTrackerStrictNamed, 'function');

    // Same identity across all access paths.
    assert.strictEqual(strictMod.Assert, assert.Assert);
    assert.strictEqual(strictMod.CallTracker, assert.CallTracker);
    assert.strictEqual(AssertStrictNamed, assert.Assert);
    assert.strictEqual(CallTrackerStrictNamed, assert.CallTracker);

    // Strict CallTracker still works.
    const tracker = new strictMod.CallTracker();
    assert.ok(Array.isArray(tracker.report()));
    assert.strictEqual(tracker.verify(), undefined);
    assert.strictEqual(tracker.reset(), undefined);
  },
};
