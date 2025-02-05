// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT ORs
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

/* TODO: the following is adopted code, enabling linting one day */
/* eslint-disable */

import {
  deepEqual,
  deepStrictEqual,
  doesNotMatch,
  doesNotReject,
  doesNotThrow,
  equal,
  fail,
  ifError,
  match,
  notDeepEqual,
  notDeepStrictEqual,
  notEqual,
  notStrictEqual,
  ok,
  rejects,
  strictEqual,
  throws,
  AssertionError,
} from 'node:assert';

import { mock } from 'node:test';

import { default as assert } from 'node:assert';

const strictEqualMessageStart = 'Expected values to be strictly equal:\n';
const start = 'Expected values to be strictly deep-equal:';
const actExp = '+ actual - expected';

function thrower(errorConstructor) {
  throw new errorConstructor({});
}

export const test_ok = {
  test(ctrl, env, ctx) {
    // truthy values just work
    [true, 1, [], {}, 'hello'].forEach(ok);
    [true, 1, [], {}, 'hello'].forEach(assert);
    [true, 1, [], {}, 'hello'].forEach((val) => {
      doesNotThrow(() => ok(val));
    });

    // falsy values throw
    [false, 0, '', undefined, NaN, null].forEach((val) => {
      throws(() => ok(val), { name: 'AssertionError' });
    });

    // falsy values throw
    [false, 0, '', undefined, NaN, null].forEach((val) => {
      throws(() => ok(val, 'message'), { message: 'message' });
    });
  },
};

export const test_equal = {
  test(ctrl, env, ctx) {
    [
      { a: 1, b: 1 },
      { a: 1, b: '1', fails: true },
      { a: 1, b: '1', fails: true, message: 'boom' },
      { a: 'a', b: 'a' },
      { a: ctx, b: ctx },
    ].forEach(({ a, b, fails, message }) => {
      if (!fails) {
        equal(a, b);
        strictEqual(a, b);
        if (message) {
          throws(() => notEqual(a, b, message), { message });
          throws(() => notStrictEqual(a, b, message), { message });
        } else {
          throws(() => notEqual(a, b), { name: 'AssertionError' });
          throws(() => notStrictEqual(a, b), { name: 'AssertionError' });
        }
      } else {
        notEqual(a, b);
        notStrictEqual(a, b);
        if (message) {
          throws(() => equal(a, b, message), { message });
          throws(() => strictEqual(a, b, message), { message });
        } else {
          throws(() => equal(a, b), { name: 'AssertionError' });
          throws(() => strictEqual(a, b), { name: 'AssertionError' });
        }
      }
    });
  },
};

export const test_fail = {
  test(ctrl, env, ctx) {
    throws(() => fail('boom'), { message: 'boom' });
    throws(() => ifError('boom'));
    throws(() => ifError(false));
    doesNotThrow(() => ifError(null));
    doesNotThrow(() => ifError(undefined));
  },
};

export const test_rejects = {
  async test(ctrl, env, ctx) {
    await rejects(Promise.reject(new Error('boom')), { message: 'boom' });
    await doesNotReject(Promise.resolve(1));
  },
};

export const test_matching = {
  test(ctrl, env, ctx) {
    match('hello', /hello/);
    throws(() => match('hello', /not/), { name: 'AssertionError' });
    doesNotMatch('hello', /not/);
    throws(() => doesNotMatch('hello', /hello/), { name: 'AssertionError' });
  },
};

export const test_deep_equal = {
  test(ctrl, env, ctx) {
    const a = {
      b: [
        {
          c: new Uint8Array([1, 2, 3]),
          d: false,
          e: 'hello',
        },
      ],
    };
    const b = {
      b: [
        {
          c: new Uint8Array([1, 2, 3]),
          d: false,
          e: 'hello',
        },
      ],
    };
    deepEqual(a, b);
    deepStrictEqual(a, b);
    b.b[0].c[0] = 4;
    notDeepEqual(a, b);
    notDeepStrictEqual(a, b);
  },
};

export const test_deep_equal_errors = {
  test(ctrl, env, ctx) {
    const a = {
      b: [
        {
          c: new Uint8Array([1, 2, 3]),
          d: false,
          e: 'hello',
        },
      ],
    };
    const b = {
      b: [
        {
          c: new Uint8Array([1, 2, 4]),
          d: true,
          e: 'hello',
        },
      ],
    };

    {
      const expectedError = [
        `Expected values to be strictly deep-equal:`,
        `+ actual - expected`,
        `+ { b: [ { c: Uint8Array(3) [ 1, 2, 3 ], d: false, e: 'hello' } ] }`,
        `- { b: [ { c: Uint8Array(3) [ 1, 2, 4 ], d: true, e: 'hello' } ] }`,
      ];
      throws(
        () => deepEqual(a, b),
        (err) => {
          assert(err instanceof AssertionError);
          deepEqual(trimMessage(err.message), expectedError);
          return true;
        }
      );
      throws(
        () => deepStrictEqual(a, b),
        (err) => {
          assert(err instanceof AssertionError);
          deepEqual(trimMessage(err.message), expectedError);
          return true;
        }
      );
    }

    // Fix one value, message updates
    b.b[0].c[2] = 3;

    {
      const expectedError = [
        `Expected values to be strictly deep-equal:`,
        `+ actual - expected`,
        `+ { b: [ { c: Uint8Array(3) [ 1, 2, 3 ], d: false, e: 'hello' } ] }`,
        `- { b: [ { c: Uint8Array(3) [ 1, 2, 3 ], d: true, e: 'hello' } ] }`,
      ];
      throws(
        () => deepEqual(a, b),
        (err) => {
          assert(err instanceof AssertionError);
          deepEqual(trimMessage(err.message), expectedError);
          return true;
        }
      );
      throws(
        () => deepStrictEqual(a, b),
        (err) => {
          assert(err instanceof AssertionError);
          deepEqual(trimMessage(err.message), expectedError);
          return true;
        }
      );
    }
  },
};

function trimMessage(msg) {
  return msg
    .trim()
    .split('\n')
    .map((line) => line.trim())
    .filter(Boolean);
}

export const test_mocks = {
  test() {
    const fn = mock.fn(() => {});
    fn(1, 2, 3);
    strictEqual(fn.mock.callCount(), 1);
    strictEqual(fn.mock.calls[0].arguments.length, 3);
    deepStrictEqual(fn.mock.calls[0].arguments, [1, 2, 3]);

    fn.mock.mockImplementation(() => 42);
    strictEqual(fn(), 42);
    strictEqual(fn.mock.callCount(), 2);

    fn.mock.resetCalls();
    strictEqual(fn.mock.callCount(), 0);
  },
};

export const spiesOnFunction = {
  test() {
    const sum = mock.fn((arg1, arg2) => {
      return arg1 + arg2;
    });

    strictEqual(sum.mock.calls.length, 0);
    strictEqual(sum(3, 4), 7);
    strictEqual(sum.call(1000, 9, 1), 10);
    strictEqual(sum.mock.calls.length, 2);

    let call = sum.mock.calls[0];
    deepStrictEqual(call.arguments, [3, 4]);
    strictEqual(call.error, undefined);
    strictEqual(call.result, 7);
    strictEqual(call.target, undefined);
    strictEqual(call.this, undefined);

    call = sum.mock.calls[1];
    deepStrictEqual(call.arguments, [9, 1]);
    strictEqual(call.error, undefined);
    strictEqual(call.result, 10);
    strictEqual(call.target, undefined);
    strictEqual(call.this, 1000);
  },
};

export const speiesOnBoundFunction = {
  test() {
    const bound = function (arg1, arg2) {
      return this + arg1 + arg2;
    }.bind(50);
    const sum = mock.fn(bound);

    strictEqual(sum.mock.calls.length, 0);
    strictEqual(sum(3, 4), 57);
    strictEqual(sum(9, 1), 60);
    strictEqual(sum.mock.calls.length, 2);

    let call = sum.mock.calls[0];
    deepStrictEqual(call.arguments, [3, 4]);
    strictEqual(call.result, 57);
    strictEqual(call.target, undefined);
    strictEqual(call.this, undefined);

    call = sum.mock.calls[1];
    deepStrictEqual(call.arguments, [9, 1]);
    strictEqual(call.result, 60);
    strictEqual(call.target, undefined);
    strictEqual(call.this, undefined);
  },
};

export const spiesOnConstructor = {
  test() {
    class ParentClazz {
      constructor(c) {
        this.c = c;
      }
    }

    class Clazz extends ParentClazz {
      #privateValue;

      constructor(a, b) {
        super(a + b);
        this.a = a;
        this.#privateValue = b;
      }

      getPrivateValue() {
        return this.#privateValue;
      }
    }

    const ctor = mock.fn(Clazz);
    const instance = new ctor(42, 85);

    ok(instance instanceof Clazz);
    ok(instance instanceof ParentClazz);
    strictEqual(instance.a, 42);
    strictEqual(instance.getPrivateValue(), 85);
    strictEqual(instance.c, 127);
    strictEqual(ctor.mock.calls.length, 1);

    const call = ctor.mock.calls[0];

    deepStrictEqual(call.arguments, [42, 85]);
    strictEqual(call.error, undefined);
    strictEqual(call.result, instance);
    strictEqual(call.target, Clazz);
    strictEqual(call.this, instance);
  },
};

export const noopSpyCreatedByDefault = {
  test() {
    const fn = mock.fn();

    strictEqual(fn.mock.calls.length, 0);
    strictEqual(fn(3, 4), undefined);
    strictEqual(fn.mock.calls.length, 1);

    const call = fn.mock.calls[0];
    deepStrictEqual(call.arguments, [3, 4]);
    strictEqual(call.result, undefined);
    strictEqual(call.target, undefined);
    strictEqual(call.this, undefined);
  },
};

export const internalNoOpFunctionCanBeReused = {
  test() {
    const fn1 = mock.fn();
    fn1.prop = true;
    const fn2 = mock.fn();

    fn1(1);
    fn2(2);
    fn1(3);

    notStrictEqual(fn1.mock, fn2.mock);
    strictEqual(fn1.mock.calls.length, 2);
    strictEqual(fn2.mock.calls.length, 1);
    strictEqual(fn1.prop, true);
    strictEqual(fn2.prop, undefined);
  },
};

export const functionsCanBeMockedMultipleTimesAtOnce = {
  test() {
    function sum(a, b) {
      return a + b;
    }

    function difference(a, b) {
      return a - b;
    }

    function product(a, b) {
      return a * b;
    }

    const fn1 = mock.fn(sum, difference);
    const fn2 = mock.fn(sum, product);

    strictEqual(fn1(5, 3), 2);
    strictEqual(fn2(5, 3), 15);
    strictEqual(fn2(4, 2), 8);
    ok(!('mock' in sum));
    ok(!('mock' in difference));
    ok(!('mock' in product));
    notStrictEqual(fn1.mock, fn2.mock);
    strictEqual(fn1.mock.calls.length, 1);
    strictEqual(fn2.mock.calls.length, 2);
  },
};

export const internalNoopFunctionCanBeReusedAsMethods = {
  test() {
    const obj = {
      _foo: 5,
      _bar: 9,
      foo() {
        return this._foo;
      },
      bar() {
        return this._bar;
      },
    };

    mock.method(obj, 'foo');
    obj.foo.prop = true;
    mock.method(obj, 'bar');
    strictEqual(obj.foo(), 5);
    strictEqual(obj.bar(), 9);
    strictEqual(obj.bar(), 9);
    notStrictEqual(obj.foo.mock, obj.bar.mock);
    strictEqual(obj.foo.mock.calls.length, 1);
    strictEqual(obj.bar.mock.calls.length, 2);
    strictEqual(obj.foo.prop, true);
    strictEqual(obj.bar.prop, undefined);
  },
};

export const methodsCanBeMockedMultipleTimesButNotAtTheSameTime = {
  test() {
    const obj = {
      offset: 3,
      sum(a, b) {
        return this.offset + a + b;
      },
    };

    function difference(a, b) {
      return this.offset + (a - b);
    }

    function product(a, b) {
      return this.offset + a * b;
    }

    const originalSum = obj.sum;
    const fn1 = mock.method(obj, 'sum', difference);

    strictEqual(obj.sum(5, 3), 5);
    strictEqual(obj.sum(5, 1), 7);
    strictEqual(obj.sum, fn1);
    notStrictEqual(fn1.mock, undefined);
    strictEqual(originalSum.mock, undefined);
    strictEqual(difference.mock, undefined);
    strictEqual(product.mock, undefined);
    strictEqual(fn1.mock.calls.length, 2);

    const fn2 = mock.method(obj, 'sum', product);

    strictEqual(obj.sum(5, 3), 18);
    strictEqual(obj.sum, fn2);
    notStrictEqual(fn1, fn2);
    strictEqual(fn2.mock.calls.length, 1);

    obj.sum.mock.restore();
    strictEqual(obj.sum, fn1);
    obj.sum.mock.restore();
    strictEqual(obj.sum, originalSum);
    strictEqual(obj.sum.mock, undefined);
  },
};

export const spiesOnObjectMethod = {
  test() {
    const obj = {
      prop: 5,
      method(a, b) {
        return a + b + this.prop;
      },
    };

    strictEqual(obj.method(1, 3), 9);
    mock.method(obj, 'method');
    strictEqual(obj.method.mock.calls.length, 0);
    strictEqual(obj.method(1, 3), 9);

    const call = obj.method.mock.calls[0];

    deepStrictEqual(call.arguments, [1, 3]);
    strictEqual(call.result, 9);
    strictEqual(call.target, undefined);
    strictEqual(call.this, obj);

    strictEqual(obj.method.mock.restore(), undefined);
    strictEqual(obj.method(1, 3), 9);
    strictEqual(obj.method.mock, undefined);
  },
};

export const spiesOnGetter = {
  test() {
    const obj = {
      prop: 5,
      get method() {
        return this.prop;
      },
    };

    strictEqual(obj.method, 5);

    const getter = mock.method(obj, 'method', { getter: true });

    strictEqual(getter.mock.calls.length, 0);
    strictEqual(obj.method, 5);

    const call = getter.mock.calls[0];

    deepStrictEqual(call.arguments, []);
    strictEqual(call.result, 5);
    strictEqual(call.target, undefined);
    strictEqual(call.this, obj);

    strictEqual(getter.mock.restore(), undefined);
    strictEqual(obj.method, 5);
  },
};

export const spiesOnSetter = {
  test() {
    const obj = {
      prop: 100,
      // eslint-disable-next-line accessor-pairs
      set method(val) {
        this.prop = val;
      },
    };

    strictEqual(obj.prop, 100);
    obj.method = 88;
    strictEqual(obj.prop, 88);

    const setter = mock.method(obj, 'method', { setter: true });

    strictEqual(setter.mock.calls.length, 0);
    obj.method = 77;
    strictEqual(obj.prop, 77);
    strictEqual(setter.mock.calls.length, 1);

    const call = setter.mock.calls[0];

    deepStrictEqual(call.arguments, [77]);
    strictEqual(call.result, undefined);
    strictEqual(call.target, undefined);
    strictEqual(call.this, obj);

    strictEqual(setter.mock.restore(), undefined);
    strictEqual(obj.prop, 77);
    obj.method = 65;
    strictEqual(obj.prop, 65);
  },
};

export const spyFunctionsCanBeBound = {
  test() {
    const sum = mock.fn(function (arg1, arg2) {
      return this + arg1 + arg2;
    });
    const bound = sum.bind(1000);

    strictEqual(bound(9, 1), 1010);
    strictEqual(sum.mock.calls.length, 1);

    const call = sum.mock.calls[0];
    deepStrictEqual(call.arguments, [9, 1]);
    strictEqual(call.result, 1010);
    strictEqual(call.target, undefined);
    strictEqual(call.this, 1000);

    strictEqual(sum.mock.restore(), undefined);
    strictEqual(sum.bind(0)(2, 11), 13);
  },
};

export const mocksPrototypeMethodsOnAnInstance = {
  async test() {
    class Runner {
      async someTask(msg) {
        return Promise.resolve(msg);
      }

      async method(msg) {
        await this.someTask(msg);
        return msg;
      }
    }
    const msg = 'ok';
    const obj = new Runner();
    strictEqual(await obj.method(msg), msg);

    mock.method(obj, obj.someTask.name);
    strictEqual(obj.someTask.mock.calls.length, 0);

    strictEqual(await obj.method(msg), msg);

    const call = obj.someTask.mock.calls[0];

    deepStrictEqual(call.arguments, [msg]);
    strictEqual(await call.result, msg);
    strictEqual(call.target, undefined);
    strictEqual(call.this, obj);

    const obj2 = new Runner();
    // Ensure that a brand new instance is not mocked
    strictEqual(obj2.someTask.mock, undefined);

    strictEqual(obj.someTask.mock.restore(), undefined);
    strictEqual(await obj.method(msg), msg);
    strictEqual(obj.someTask.mock, undefined);
    strictEqual(Runner.prototype.someTask.mock, undefined);
  },
};

export const spiesOnAsyncStaticClassMethods = {
  async test() {
    class Runner {
      static async someTask(msg) {
        return Promise.resolve(msg);
      }

      static async method(msg) {
        await this.someTask(msg);
        return msg;
      }
    }
    const msg = 'ok';
    strictEqual(await Runner.method(msg), msg);

    mock.method(Runner, Runner.someTask.name);
    strictEqual(Runner.someTask.mock.calls.length, 0);

    strictEqual(await Runner.method(msg), msg);

    const call = Runner.someTask.mock.calls[0];

    deepStrictEqual(call.arguments, [msg]);
    strictEqual(await call.result, msg);
    strictEqual(call.target, undefined);
    strictEqual(call.this, Runner);

    strictEqual(Runner.someTask.mock.restore(), undefined);
    strictEqual(await Runner.method(msg), msg);
    strictEqual(Runner.someTask.mock, undefined);
    strictEqual(Runner.prototype.someTask, undefined);
  },
};

export const givenNullToAMockMethodItThrowsAInvalidArgumentError = {
  test() {
    throws(() => mock.method(null, {}), { code: 'ERR_INVALID_ARG_TYPE' });
  },
};

export const itShouldThrowGivenAnInexistentPropertyOnAObjectInstance = {
  test() {
    throws(() => mock.method({ abc: 0 }, 'non-existent'), {
      code: 'ERR_INVALID_ARG_VALUE',
    });
  },
};

export const spyFunctionsCanBeUsedOnClassesInheritance = {
  test() {
    // Makes sure that having a null-prototype doesn't throw our system off
    class A extends null {
      static someTask(msg) {
        return msg;
      }
      static method(msg) {
        return this.someTask(msg);
      }
    }
    class B extends A {}
    class C extends B {}

    const msg = 'ok';
    strictEqual(C.method(msg), msg);

    mock.method(C, C.someTask.name);
    strictEqual(C.someTask.mock.calls.length, 0);

    strictEqual(C.method(msg), msg);

    const call = C.someTask.mock.calls[0];

    deepStrictEqual(call.arguments, [msg]);
    strictEqual(call.result, msg);
    strictEqual(call.target, undefined);
    strictEqual(call.this, C);

    strictEqual(C.someTask.mock.restore(), undefined);
    strictEqual(C.method(msg), msg);
    strictEqual(C.someTask.mock, undefined);
  },
};

export const spyFunctionsDontAffectThePrototypeChain = {
  test() {
    class A {
      static someTask(msg) {
        return msg;
      }
    }
    class B extends A {}
    class C extends B {}

    const msg = 'ok';

    const ABeforeMockIsUnchanged = Object.getOwnPropertyDescriptor(
      A,
      A.someTask.name
    );
    const BBeforeMockIsUnchanged = Object.getOwnPropertyDescriptor(
      B,
      B.someTask.name
    );
    const CBeforeMockShouldNotHaveDesc = Object.getOwnPropertyDescriptor(
      C,
      C.someTask.name
    );

    mock.method(C, C.someTask.name);
    C.someTask(msg);
    const BAfterMockIsUnchanged = Object.getOwnPropertyDescriptor(
      B,
      B.someTask.name
    );

    const AAfterMockIsUnchanged = Object.getOwnPropertyDescriptor(
      A,
      A.someTask.name
    );
    const CAfterMockHasDescriptor = Object.getOwnPropertyDescriptor(
      C,
      C.someTask.name
    );

    strictEqual(CBeforeMockShouldNotHaveDesc, undefined);
    ok(CAfterMockHasDescriptor);

    deepStrictEqual(ABeforeMockIsUnchanged, AAfterMockIsUnchanged);
    strictEqual(BBeforeMockIsUnchanged, BAfterMockIsUnchanged);
    strictEqual(BBeforeMockIsUnchanged, undefined);

    strictEqual(C.someTask.mock.restore(), undefined);
    const CAfterRestoreKeepsDescriptor = Object.getOwnPropertyDescriptor(
      C,
      C.someTask.name
    );
    ok(CAfterRestoreKeepsDescriptor);
  },
};

export const mockedFunctionsReportThrownErrors = {
  test() {
    const testError = new Error('test error');
    const fn = mock.fn(() => {
      throw testError;
    });

    throws(fn, /test error/);
    strictEqual(fn.mock.calls.length, 1);

    const call = fn.mock.calls[0];

    deepStrictEqual(call.arguments, []);
    strictEqual(call.error, testError);
    strictEqual(call.result, undefined);
    strictEqual(call.target, undefined);
    strictEqual(call.this, undefined);
  },
};

export const mockedConstructorsReportThrownErrors = {
  test() {
    const testError = new Error('test error');
    class Clazz {
      constructor() {
        throw testError;
      }
    }

    const ctor = mock.fn(Clazz);

    throws(() => {
      new ctor();
    }, /test error/);
    strictEqual(ctor.mock.calls.length, 1);

    const call = ctor.mock.calls[0];

    deepStrictEqual(call.arguments, []);
    strictEqual(call.error, testError);
    strictEqual(call.result, undefined);
    strictEqual(call.target, Clazz);
    strictEqual(call.this, undefined);
  },
};

export const mocksAFunction = {
  test() {
    const sum = (arg1, arg2) => arg1 + arg2;
    const difference = (arg1, arg2) => arg1 - arg2;
    const fn = mock.fn(sum, difference);

    strictEqual(fn.mock.calls.length, 0);
    strictEqual(fn(3, 4), -1);
    strictEqual(fn(9, 1), 8);
    strictEqual(fn.mock.calls.length, 2);

    let call = fn.mock.calls[0];
    deepStrictEqual(call.arguments, [3, 4]);
    strictEqual(call.result, -1);
    strictEqual(call.target, undefined);
    strictEqual(call.this, undefined);

    call = fn.mock.calls[1];
    deepStrictEqual(call.arguments, [9, 1]);
    strictEqual(call.result, 8);
    strictEqual(call.target, undefined);
    strictEqual(call.this, undefined);

    strictEqual(fn.mock.restore(), undefined);
    strictEqual(fn(2, 11), 13);
  },
};

export const mocksAConstructor = {
  test() {
    class ParentClazz {
      constructor(c) {
        this.c = c;
      }
    }

    class Clazz extends ParentClazz {
      #privateValue;

      constructor(a, b) {
        super(a + b);
        this.a = a;
        this.#privateValue = b;
      }

      getPrivateValue() {
        return this.#privateValue;
      }
    }

    class MockClazz {
      #privateValue;

      constructor(z) {
        this.z = z;
      }
    }

    const ctor = mock.fn(Clazz, MockClazz);
    const instance = new ctor(42, 85);

    ok(!(instance instanceof MockClazz));
    ok(instance instanceof Clazz);
    ok(instance instanceof ParentClazz);
    strictEqual(instance.a, undefined);
    strictEqual(instance.c, undefined);
    strictEqual(instance.z, 42);
    strictEqual(ctor.mock.calls.length, 1);

    const call = ctor.mock.calls[0];

    deepStrictEqual(call.arguments, [42, 85]);
    strictEqual(call.result, instance);
    strictEqual(call.target, Clazz);
    strictEqual(call.this, instance);
    throws(() => {
      instance.getPrivateValue();
    }, /TypeError: Cannot read private member #privateValue /);
  },
};

export const mocksAnObjectMethod = {
  test() {
    const obj = {
      prop: 5,
      method(a, b) {
        return a + b + this.prop;
      },
    };

    function mockMethod(a) {
      return a + this.prop;
    }

    strictEqual(obj.method(1, 3), 9);
    mock.method(obj, 'method', mockMethod);
    strictEqual(obj.method.mock.calls.length, 0);
    strictEqual(obj.method(1, 3), 6);

    const call = obj.method.mock.calls[0];

    deepStrictEqual(call.arguments, [1, 3]);
    strictEqual(call.result, 6);
    strictEqual(call.target, undefined);
    strictEqual(call.this, obj);

    strictEqual(obj.method.mock.restore(), undefined);
    strictEqual(obj.method(1, 3), 9);
    strictEqual(obj.method.mock, undefined);
  },
};

export const mocksAGetter = {
  test() {
    const obj = {
      prop: 5,
      get method() {
        return this.prop;
      },
    };

    function mockMethod() {
      return this.prop - 1;
    }

    strictEqual(obj.method, 5);

    const getter = mock.method(obj, 'method', mockMethod, { getter: true });

    strictEqual(getter.mock.calls.length, 0);
    strictEqual(obj.method, 4);

    const call = getter.mock.calls[0];

    deepStrictEqual(call.arguments, []);
    strictEqual(call.result, 4);
    strictEqual(call.target, undefined);
    strictEqual(call.this, obj);

    strictEqual(getter.mock.restore(), undefined);
    strictEqual(obj.method, 5);
  },
};

export const mocksASetter = {
  test() {
    const obj = {
      prop: 100,
      // eslint-disable-next-line accessor-pairs
      set method(val) {
        this.prop = val;
      },
    };

    function mockMethod(val) {
      this.prop = -val;
    }

    strictEqual(obj.prop, 100);
    obj.method = 88;
    strictEqual(obj.prop, 88);

    const setter = mock.method(obj, 'method', mockMethod, { setter: true });

    strictEqual(setter.mock.calls.length, 0);
    obj.method = 77;
    strictEqual(obj.prop, -77);
    strictEqual(setter.mock.calls.length, 1);

    const call = setter.mock.calls[0];

    deepStrictEqual(call.arguments, [77]);
    strictEqual(call.result, undefined);
    strictEqual(call.target, undefined);
    strictEqual(call.this, obj);

    strictEqual(setter.mock.restore(), undefined);
    strictEqual(obj.prop, -77);
    obj.method = 65;
    strictEqual(obj.prop, 65);
  },
};

export const mocksAGetterWithSyntaxSugar = {
  test() {
    const obj = {
      prop: 5,
      get method() {
        return this.prop;
      },
    };

    function mockMethod() {
      return this.prop - 1;
    }
    const getter = mock.getter(obj, 'method', mockMethod);
    strictEqual(getter.mock.calls.length, 0);
    strictEqual(obj.method, 4);

    const call = getter.mock.calls[0];

    deepStrictEqual(call.arguments, []);
    strictEqual(call.result, 4);
    strictEqual(call.target, undefined);
    strictEqual(call.this, obj);

    strictEqual(getter.mock.restore(), undefined);
    strictEqual(obj.method, 5);
  },
};

export const mocksASetterWithSyntaxSugar = {
  test() {
    const obj = {
      prop: 100,
      // eslint-disable-next-line accessor-pairs
      set method(val) {
        this.prop = val;
      },
    };

    function mockMethod(val) {
      this.prop = -val;
    }

    strictEqual(obj.prop, 100);
    obj.method = 88;
    strictEqual(obj.prop, 88);

    const setter = mock.setter(obj, 'method', mockMethod);

    strictEqual(setter.mock.calls.length, 0);
    obj.method = 77;
    strictEqual(obj.prop, -77);
    strictEqual(setter.mock.calls.length, 1);

    const call = setter.mock.calls[0];

    deepStrictEqual(call.arguments, [77]);
    strictEqual(call.result, undefined);
    strictEqual(call.target, undefined);
    strictEqual(call.this, obj);

    strictEqual(setter.mock.restore(), undefined);
    strictEqual(obj.prop, -77);
    obj.method = 65;
    strictEqual(obj.prop, 65);
  },
};

export const mockedFunctionsMatchNameAndLength = {
  test() {
    function getNameAndLength(fn) {
      return {
        name: Object.getOwnPropertyDescriptor(fn, 'name'),
        length: Object.getOwnPropertyDescriptor(fn, 'length'),
      };
    }

    function func1() {}
    const func2 = function (a) {}; // eslint-disable-line func-style
    const arrow = (a, b, c) => {};
    const obj = { method(a, b) {} };

    deepStrictEqual(getNameAndLength(func1), getNameAndLength(mock.fn(func1)));
    deepStrictEqual(getNameAndLength(func2), getNameAndLength(mock.fn(func2)));
    deepStrictEqual(getNameAndLength(arrow), getNameAndLength(mock.fn(arrow)));
    deepStrictEqual(
      getNameAndLength(obj.method),
      getNameAndLength(mock.method(obj, 'method', func1))
    );
  },
};

export const methodFailsIfMethodCannotBeRedefined = {
  test() {
    const obj = {
      prop: 5,
    };

    Object.defineProperty(obj, 'method', {
      configurable: false,
      value(a, b) {
        return a + b + this.prop;
      },
    });

    function mockMethod(a) {
      return a + this.prop;
    }

    throws(() => {
      mock.method(obj, 'method', mockMethod);
    }, /Cannot redefine property: method/);
    strictEqual(obj.method(1, 3), 9);
    strictEqual(obj.method.mock, undefined);
  },
};

export const methodFailsIfFieldIsAPropertyInsteadOfAMethod = {
  test() {
    const obj = {
      prop: 5,
      method: 100,
    };

    function mockMethod(a) {
      return a + this.prop;
    }

    throws(() => {
      mock.method(obj, 'method', mockMethod);
    }, /The argument 'methodName' must be a method/);
    strictEqual(obj.method, 100);
    strictEqual(obj.method.mock, undefined);
  },
};

export const mocksCanBeRestored = {
  test() {
    let cnt = 0;

    function addOne() {
      cnt++;
      return cnt;
    }

    function addTwo() {
      cnt += 2;
      return cnt;
    }

    const fn = mock.fn(addOne, addTwo, { times: 2 });

    strictEqual(fn(), 2);
    strictEqual(fn(), 4);
    strictEqual(fn(), 5);
    strictEqual(fn(), 6);
  },
};

export const mockImplementationCanBeChangedDynamically = {
  test() {
    let cnt = 0;

    function addOne() {
      cnt++;
      return cnt;
    }

    function addTwo() {
      cnt += 2;
      return cnt;
    }

    function addThree() {
      cnt += 3;
      return cnt;
    }

    function mustNotCall() {
      return function () {
        throw new Error('This function should not be called');
      };
    }

    const fn = mock.fn(addOne);

    strictEqual(fn.mock.callCount(), 0);
    strictEqual(fn(), 1);
    strictEqual(fn(), 2);
    strictEqual(fn(), 3);
    strictEqual(fn.mock.callCount(), 3);

    fn.mock.mockImplementation(addTwo);
    strictEqual(fn(), 5);
    strictEqual(fn(), 7);
    strictEqual(fn.mock.callCount(), 5);

    fn.mock.restore();
    strictEqual(fn(), 8);
    strictEqual(fn(), 9);
    strictEqual(fn.mock.callCount(), 7);

    throws(() => {
      fn.mock.mockImplementationOnce(mustNotCall(), 6);
    }, /The value of "onCall" is out of range\. It must be >= 7/);

    fn.mock.mockImplementationOnce(addThree, 7);
    fn.mock.mockImplementationOnce(addTwo, 8);
    strictEqual(fn(), 12);
    strictEqual(fn(), 14);
    strictEqual(fn(), 15);
    strictEqual(fn.mock.callCount(), 10);
    fn.mock.mockImplementationOnce(addThree);
    strictEqual(fn(), 18);
    strictEqual(fn(), 19);
    strictEqual(fn.mock.callCount(), 12);
  },
};

export const resetMockCalls = {
  test() {
    const sum = (arg1, arg2) => arg1 + arg2;
    const difference = (arg1, arg2) => arg1 - arg2;
    const fn = mock.fn(sum, difference);

    strictEqual(fn(1, 2), -1);
    strictEqual(fn(2, 1), 1);
    strictEqual(fn.mock.calls.length, 2);
    strictEqual(fn.mock.callCount(), 2);

    fn.mock.resetCalls();
    strictEqual(fn.mock.calls.length, 0);
    strictEqual(fn.mock.callCount(), 0);

    strictEqual(fn(3, 2), 1);
  },
};

export const usesTopLevelMock = {
  test() {
    function sum(a, b) {
      return a + b;
    }

    function difference(a, b) {
      return a - b;
    }

    const fn = mock.fn(sum, difference);

    strictEqual(fn.mock.calls.length, 0);
    strictEqual(fn(3, 4), -1);
    strictEqual(fn.mock.calls.length, 1);
    mock.reset();
    strictEqual(fn(3, 4), 7);
    strictEqual(fn.mock.calls.length, 2);
  },
};

export const theGetterAndSetterOptionsCannotBeUsedTogether = {
  test() {
    throws(() => {
      mock.method({}, 'method', { getter: true, setter: true });
    }, /The property 'options\.setter' cannot be used with 'options\.getter'/);
  },
};

export const methodNamesMustBeStringsOrSymbols = {
  test() {
    const symbol = Symbol();
    const obj = {
      method() {},
      [symbol]() {},
    };

    mock.method(obj, 'method');
    mock.method(obj, symbol);

    throws(() => {
      mock.method(obj, {});
    }, /The "methodName" argument must be one of type string or symbol/);
  },
};

export const theTimesOptionMustBeAnIntegerGreaterOrEqualToOne = {
  test() {
    throws(() => {
      mock.fn({ times: null });
    }, /The "options\.times" property must be of type number/);

    throws(() => {
      mock.fn({ times: 0 });
    }, /The value of "options\.times" is out of range/);

    throws(() => {
      mock.fn(() => {}, { times: 3.14159 });
    }, /The value of "options\.times" is out of range/);
  },
};

export const spiesOnAClassPrototypeMethod = {
  test() {
    class Clazz {
      constructor(c) {
        this.c = c;
      }

      getC() {
        return this.c;
      }
    }

    const instance = new Clazz(85);

    strictEqual(instance.getC(), 85);
    mock.method(Clazz.prototype, 'getC');

    strictEqual(instance.getC.mock.calls.length, 0);
    strictEqual(instance.getC(), 85);
    strictEqual(instance.getC.mock.calls.length, 1);
    strictEqual(Clazz.prototype.getC.mock.calls.length, 1);

    const call = instance.getC.mock.calls[0];
    deepStrictEqual(call.arguments, []);
    strictEqual(call.result, 85);
    strictEqual(call.error, undefined);
    strictEqual(call.target, undefined);
    strictEqual(call.this, instance);
  },
};

export const getterFailsIfGetterOptionsSetToFalse = {
  test() {
    throws(() => {
      mock.getter({}, 'method', { getter: false });
    }, /The property 'options\.getter' cannot be false/);
  },
};

export const setterFailsIfSetterOptionsSetToFalse = {
  test() {
    throws(() => {
      mock.setter({}, 'method', { setter: false });
    }, /The property 'options\.setter' cannot be false/);
  },
};

export const getterFailsIfSetterOptionsIsTrue = {
  test() {
    throws(() => {
      mock.getter({}, 'method', { setter: true });
    }, /The property 'options\.setter' cannot be used with 'options\.getter'/);
  },
};

export const setterFailsIfGetterOptionsIsTrue = {
  test() {
    throws(() => {
      mock.setter({}, 'method', { getter: true });
    }, /The property 'options\.setter' cannot be used with 'options\.getter'/);
  },
};
