// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Smoke test that verifies the shape of the `node:test` module matches
// Node.js: the default export IS the `test` function, with every named
// export also attached as a property on that function. All test-runner
// APIs throw `ERR_METHOD_NOT_IMPLEMENTED` since workerd has no runner.

import assert from 'node:assert';

import {
  test,
  suite,
  describe,
  it,
  before,
  after,
  beforeEach,
  afterEach,
  todo,
  skip,
  only,
  expectFailure,
  run,
  mock,
  snapshot,
  assert as testAssert,
  MockTracker,
  MockFunctionContext,
} from 'node:test';
import testDefault from 'node:test';

function assertThrowsNotImplemented(fn, label) {
  assert.throws(fn, (err) => {
    assert.strictEqual(
      err.code,
      'ERR_METHOD_NOT_IMPLEMENTED',
      `${label} should throw ERR_METHOD_NOT_IMPLEMENTED, got ${err.code}`
    );
    return true;
  });
}

export const topLevelFunctionsExist = {
  test() {
    for (const [name, fn] of Object.entries({
      test,
      suite,
      describe,
      it,
      before,
      after,
      beforeEach,
      afterEach,
      todo,
      skip,
      only,
      expectFailure,
      run,
    })) {
      assert.strictEqual(typeof fn, 'function', `${name} should be a function`);
    }
  },
};

export const topLevelFunctionsThrow = {
  test() {
    assertThrowsNotImplemented(() => test('x'), 'test');
    assertThrowsNotImplemented(() => suite('x'), 'suite');
    assertThrowsNotImplemented(() => describe('x'), 'describe');
    assertThrowsNotImplemented(() => it('x'), 'it');
    assertThrowsNotImplemented(() => before(), 'before');
    assertThrowsNotImplemented(() => after(), 'after');
    assertThrowsNotImplemented(() => beforeEach(), 'beforeEach');
    assertThrowsNotImplemented(() => afterEach(), 'afterEach');
    assertThrowsNotImplemented(() => todo('x'), 'todo');
    assertThrowsNotImplemented(() => skip('x'), 'skip');
    assertThrowsNotImplemented(() => only('x'), 'only');
    assertThrowsNotImplemented(() => expectFailure('x'), 'expectFailure');
    assertThrowsNotImplemented(() => run(), 'run');
  },
};

export const snapshotShape = {
  test() {
    assert.strictEqual(typeof snapshot, 'object');
    assert.strictEqual(
      typeof snapshot.setDefaultSnapshotSerializers,
      'function'
    );
    assert.strictEqual(typeof snapshot.setResolveSnapshotPath, 'function');
    assertThrowsNotImplemented(
      () => snapshot.setDefaultSnapshotSerializers([]),
      'snapshot.setDefaultSnapshotSerializers'
    );
    assertThrowsNotImplemented(
      () => snapshot.setResolveSnapshotPath(() => ''),
      'snapshot.setResolveSnapshotPath'
    );
    // Also reachable as test.snapshot
    assert.strictEqual(test.snapshot, snapshot);
  },
};

export const assertShape = {
  test() {
    assert.strictEqual(typeof testAssert, 'object');
    assert.strictEqual(typeof testAssert.register, 'function');
    assertThrowsNotImplemented(
      () => testAssert.register('name', () => {}),
      'assert.register'
    );
    // Also reachable as test.assert
    assert.strictEqual(test.assert, testAssert);
  },
};

export const mockTimersShape = {
  test() {
    assert.strictEqual(typeof mock.timers, 'object');
    assert.strictEqual(typeof mock.timers.enable, 'function');
    assert.strictEqual(typeof mock.timers.tick, 'function');
    assert.strictEqual(typeof mock.timers.runAll, 'function');
    assert.strictEqual(typeof mock.timers.reset, 'function');
    assert.strictEqual(typeof mock.timers.setTime, 'function');
    assertThrowsNotImplemented(
      () => mock.timers.enable(),
      'mock.timers.enable'
    );
    assertThrowsNotImplemented(() => mock.timers.tick(10), 'mock.timers.tick');
    assertThrowsNotImplemented(
      () => mock.timers.runAll(),
      'mock.timers.runAll'
    );
    assertThrowsNotImplemented(() => mock.timers.reset(), 'mock.timers.reset');
    assertThrowsNotImplemented(
      () => mock.timers.setTime(0),
      'mock.timers.setTime'
    );
  },
};

export const mockPropertyShape = {
  test() {
    assert.strictEqual(typeof mock.property, 'function');
    assertThrowsNotImplemented(
      () => mock.property({}, 'x', 1),
      'mock.property'
    );
  },
};

export const mockInstanceofStillWorks = {
  test() {
    assert.ok(
      mock instanceof MockTracker,
      'named `mock` should still be a MockTracker'
    );
    // And the existing fn() behaviour is preserved.
    const m = mock.fn();
    m();
    assert.strictEqual(m.mock.callCount(), 1);
    // Clean up so nothing leaks between tests.
    mock.reset();
  },
};

export const defaultExportIsFunction = {
  test() {
    assert.strictEqual(typeof testDefault, 'function');
    assert.strictEqual(testDefault, test);
    // Shared property references
    assert.strictEqual(testDefault.describe, describe);
    assert.strictEqual(testDefault.it, it);
    assert.strictEqual(testDefault.mock, mock);
    assert.strictEqual(testDefault.MockTracker, MockTracker);
    assert.strictEqual(testDefault.MockFunctionContext, MockFunctionContext);
    assert.strictEqual(testDefault.snapshot, snapshot);
    assert.strictEqual(testDefault.assert, testAssert);
    // Calling the default export throws
    assertThrowsNotImplemented(() => testDefault('x'), 'testDefault');
  },
};
