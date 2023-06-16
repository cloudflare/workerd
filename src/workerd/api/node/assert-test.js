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
  }
};

export const test_equal = {
  test(ctrl, env, ctx) {
    [
      {a: 1, b: 1},
      {a: 1, b: '1', fails: true},
      {a: 1, b: '1', fails: true, message: 'boom'},
      {a: 'a', b: 'a'},
      {a: ctx, b: ctx},
    ].forEach(({a,b,fails,message}) => {
      if (!fails) {
        equal(a,b);
        strictEqual(a,b);
        if (message) {
          throws(() => notEqual(a, b, message), { message });
          throws(() => notStrictEqual(a, b, message), { message });
        } else {
          throws(() => notEqual(a,b), { name: 'AssertionError' });
          throws(() => notStrictEqual(a,b), { name: 'AssertionError' });
        }
      } else {
        notEqual(a, b);
        notStrictEqual(a, b);
        if (message) {
          throws(() => equal(a,b,message), { message });
          throws(() => strictEqual(a,b,message), { message });
        } else {
          throws(() => equal(a,b), { name: 'AssertionError' });
          throws(() => strictEqual(a,b), { name: 'AssertionError' });
        }
      }
    });
  }
};

export const test_fail = {
  test(ctrl, env, ctx) {
    throws(() => fail("boom"), { message: "boom" });
    throws(() => ifError("boom"));
    throws(() => ifError(false));
    doesNotThrow(() => ifError(null));
    doesNotThrow(() => ifError(undefined));
  }
};

export const test_rejects = {
  async test(ctrl, env, ctx) {
    await rejects(Promise.reject(new Error('boom')), { message: 'boom' });
    await doesNotReject(Promise.resolve(1));
  }
};

export const test_matching = {
  test(ctrl, env, ctx) {
    match('hello', /hello/);
    throws(() => match('hello', /not/), { name: 'AssertionError' });
    doesNotMatch('hello', /not/);
    throws(() => doesNotMatch('hello', /hello/), { name: 'AssertionError' });
  }
};

export const test_deep_equal = {
  test(ctrl, env, ctx) {
    const a = {
      b: [
        {
          c: new Uint8Array([1,2,3]),
          d: false,
          e: 'hello'
        }
      ],
    };
    const b = {
      b: [
        {
          c: new Uint8Array([1,2,3]),
          d: false,
          e: 'hello'
        }
      ],
    };
    deepEqual(a,b);
    deepStrictEqual(a,b);
    b.b[0].c[0] = 4;
    notDeepEqual(a,b);
    notDeepStrictEqual(a,b);
  }
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
          return true
        }
      );
      throws(
        () => deepStrictEqual(a, b),
        (err) => {
          assert(err instanceof AssertionError);
          deepEqual(trimMessage(err.message), expectedError);
          return true
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
          return true
        }
      );
      throws(
        () => deepStrictEqual(a, b),
        (err) => {
          assert(err instanceof AssertionError);
          deepEqual(trimMessage(err.message), expectedError);
          return true
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
