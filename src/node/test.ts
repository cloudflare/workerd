// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import { MockTracker, MockFunctionContext } from 'node-internal:mock';

// The workerd runtime has no `node:test` test runner, so every test-runner API
// throws ERR_METHOD_NOT_IMPLEMENTED. The shape of the module mirrors Node.js:
// the default export is the `test` function itself, with all other public
// symbols attached as properties on that function.

function test(_name?: unknown, _options?: unknown, _fn?: unknown): never {
  throw new ERR_METHOD_NOT_IMPLEMENTED('test');
}
function suite(_name?: unknown, _options?: unknown, _fn?: unknown): never {
  throw new ERR_METHOD_NOT_IMPLEMENTED('suite');
}
function describe(_name?: unknown, _options?: unknown, _fn?: unknown): never {
  throw new ERR_METHOD_NOT_IMPLEMENTED('describe');
}
function it(_name?: unknown, _options?: unknown, _fn?: unknown): never {
  throw new ERR_METHOD_NOT_IMPLEMENTED('it');
}
function before(_fn?: unknown, _options?: unknown): never {
  throw new ERR_METHOD_NOT_IMPLEMENTED('before');
}
function after(_fn?: unknown, _options?: unknown): never {
  throw new ERR_METHOD_NOT_IMPLEMENTED('after');
}
function beforeEach(_fn?: unknown, _options?: unknown): never {
  throw new ERR_METHOD_NOT_IMPLEMENTED('beforeEach');
}
function afterEach(_fn?: unknown, _options?: unknown): never {
  throw new ERR_METHOD_NOT_IMPLEMENTED('afterEach');
}
function todo(_name?: unknown, _options?: unknown, _fn?: unknown): never {
  throw new ERR_METHOD_NOT_IMPLEMENTED('todo');
}
function skip(_name?: unknown, _options?: unknown, _fn?: unknown): never {
  throw new ERR_METHOD_NOT_IMPLEMENTED('skip');
}
function only(_name?: unknown, _options?: unknown, _fn?: unknown): never {
  throw new ERR_METHOD_NOT_IMPLEMENTED('only');
}
function expectFailure(
  _name?: unknown,
  _options?: unknown,
  _fn?: unknown
): never {
  throw new ERR_METHOD_NOT_IMPLEMENTED('expectFailure');
}
function run(_options?: unknown): never {
  throw new ERR_METHOD_NOT_IMPLEMENTED('run');
}

const mock = new MockTracker();

// MockTracker does not currently implement `timers` or `property`; stub them
// with throwing placeholders so the surface matches Node.js.
(mock as unknown as { timers: object }).timers = {
  enable(_options?: unknown): never {
    throw new ERR_METHOD_NOT_IMPLEMENTED('mock.timers.enable');
  },
  tick(_ms?: unknown): never {
    throw new ERR_METHOD_NOT_IMPLEMENTED('mock.timers.tick');
  },
  runAll(): never {
    throw new ERR_METHOD_NOT_IMPLEMENTED('mock.timers.runAll');
  },
  reset(): never {
    throw new ERR_METHOD_NOT_IMPLEMENTED('mock.timers.reset');
  },
  setTime(_ms: unknown): never {
    throw new ERR_METHOD_NOT_IMPLEMENTED('mock.timers.setTime');
  },
};
(
  mock as unknown as {
    property: (obj: unknown, key: unknown, value?: unknown) => never;
  }
).property = (_obj: unknown, _key: unknown, _value?: unknown): never => {
  throw new ERR_METHOD_NOT_IMPLEMENTED('mock.property');
};

const snapshot = {
  setDefaultSnapshotSerializers(_serializers: unknown): never {
    throw new ERR_METHOD_NOT_IMPLEMENTED(
      'snapshot.setDefaultSnapshotSerializers'
    );
  },
  setResolveSnapshotPath(_fn: unknown): never {
    throw new ERR_METHOD_NOT_IMPLEMENTED('snapshot.setResolveSnapshotPath');
  },
};

const testAssert = {
  register(_name: unknown, _fn: unknown): never {
    throw new ERR_METHOD_NOT_IMPLEMENTED('assert.register');
  },
};

// Attach all public symbols onto the `test` function so that
// `import t from 'node:test'` gives the same shape as Node.js, where
// `module.exports = test` with everything hung off it.
Object.assign(test, {
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
  assert: testAssert,
  MockTracker,
  MockFunctionContext,
});

export {
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
  MockTracker,
  MockFunctionContext,
};
export { testAssert as assert };
export default test;
