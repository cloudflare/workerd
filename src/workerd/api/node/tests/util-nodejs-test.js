// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
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
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

import assert from 'node:assert';
import { mock } from 'node:test';
import util, { inspect } from 'node:util';

const remainingMustCallErrors = new Set();
function commonMustCall(f) {
  const error = new Error('Expected function to be called');
  remainingMustCallErrors.add(error);
  return (...args) => {
    remainingMustCallErrors.delete(error);
    return f(...args);
  };
}
function assertCalledMustCalls() {
  try {
    for (const error of remainingMustCallErrors) throw error;
  } finally {
    remainingMustCallErrors.clear();
  }
}

export const utilInspect = {
  // test-util-inspect.js
  test(ctrl, env, ctx) {
    assert.strictEqual(util.inspect(1), '1');
    assert.strictEqual(util.inspect(false), 'false');
    assert.strictEqual(util.inspect(''), "''");
    assert.strictEqual(util.inspect('hello'), "'hello'");
    assert.strictEqual(
      util.inspect(function abc() {}),
      '[Function: abc]'
    );
    assert.strictEqual(
      util.inspect(() => {}),
      '[Function (anonymous)]'
    );
    assert.strictEqual(
      util.inspect(async function () {}),
      '[AsyncFunction (anonymous)]'
    );
    assert.strictEqual(
      util.inspect(async () => {}),
      '[AsyncFunction (anonymous)]'
    );

    // Special function inspection.
    {
      const fn = (() => function* () {})();
      assert.strictEqual(util.inspect(fn), '[GeneratorFunction (anonymous)]');
      assert.strictEqual(
        util.inspect(async function* abc() {}),
        '[AsyncGeneratorFunction: abc]'
      );
      Object.setPrototypeOf(
        fn,
        Object.getPrototypeOf(async () => {})
      );
      assert.strictEqual(
        util.inspect(fn),
        '[GeneratorFunction (anonymous)] AsyncFunction'
      );
      Object.defineProperty(fn, 'name', { value: 5, configurable: true });
      assert.strictEqual(
        util.inspect(fn),
        '[GeneratorFunction: 5] AsyncFunction'
      );
      Object.defineProperty(fn, Symbol.toStringTag, {
        value: 'Foobar',
        configurable: true,
      });
      assert.strictEqual(
        util.inspect({ ['5']: fn }),
        "{ '5': [GeneratorFunction: 5] AsyncFunction [Foobar] }"
      );
      Object.defineProperty(fn, 'name', { value: '5', configurable: true });
      Object.setPrototypeOf(fn, null);
      assert.strictEqual(
        util.inspect(fn),
        '[GeneratorFunction (null prototype): 5] [Foobar]'
      );
      assert.strictEqual(
        util.inspect({ ['5']: fn }),
        "{ '5': [GeneratorFunction (null prototype): 5] [Foobar] }"
      );
    }

    assert.strictEqual(util.inspect(undefined), 'undefined');
    assert.strictEqual(util.inspect(null), 'null');
    assert.strictEqual(util.inspect(/foo(bar\n)?/gi), '/foo(bar\\n)?/gi');
    assert.strictEqual(
      util.inspect(new Date('Sun, 14 Feb 2010 11:48:40 GMT')),
      new Date('2010-02-14T12:48:40+01:00').toISOString()
    );
    assert.strictEqual(util.inspect(new Date('')), new Date('').toString());
    assert.strictEqual(util.inspect('\n\x01'), "'\\n\\x01'");
    assert.strictEqual(
      util.inspect(`${Array(75).fill(1)}'\n\x1d\n\x03\x85\x7f\x7e\x9f\xa0`),
      // eslint-disable-next-line no-irregular-whitespace
      `"${Array(75).fill(1)}'\\n" +\n  '\\x1D\\n' +\n  '\\x03\\x85\\x7F~\\x9F '`
    );
    assert.strictEqual(util.inspect([]), '[]');
    assert.strictEqual(util.inspect({ __proto__: [] }), 'Array {}');
    assert.strictEqual(util.inspect([1, 2]), '[ 1, 2 ]');
    assert.strictEqual(util.inspect([1, [2, 3]]), '[ 1, [ 2, 3 ] ]');
    assert.strictEqual(util.inspect({}), '{}');
    assert.strictEqual(util.inspect({ a: 1 }), '{ a: 1 }');
    assert.strictEqual(
      util.inspect({ a: function () {} }),
      '{ a: [Function: a] }'
    );
    assert.strictEqual(util.inspect({ a: () => {} }), '{ a: [Function: a] }');
    // eslint-disable-next-line func-name-matching
    assert.strictEqual(
      util.inspect({ a: async function abc() {} }),
      '{ a: [AsyncFunction: abc] }'
    );
    assert.strictEqual(
      util.inspect({ a: async () => {} }),
      '{ a: [AsyncFunction: a] }'
    );
    assert.strictEqual(
      util.inspect({ a: function* () {} }),
      '{ a: [GeneratorFunction: a] }'
    );
    assert.strictEqual(util.inspect({ a: 1, b: 2 }), '{ a: 1, b: 2 }');
    assert.strictEqual(util.inspect({ a: {} }), '{ a: {} }');
    assert.strictEqual(util.inspect({ a: { b: 2 } }), '{ a: { b: 2 } }');
    assert.strictEqual(
      util.inspect({ a: { b: { c: { d: 2 } } } }),
      '{ a: { b: { c: [Object] } } }'
    );
    assert.strictEqual(
      util.inspect({ a: { b: { c: { d: 2 } } } }, false, null),
      '{\n  a: { b: { c: { d: 2 } } }\n}'
    );
    assert.strictEqual(
      util.inspect([1, 2, 3], true),
      '[ 1, 2, 3, [length]: 3 ]'
    );
    assert.strictEqual(
      util.inspect({ a: { b: { c: 2 } } }, false, 0),
      '{ a: [Object] }'
    );
    assert.strictEqual(
      util.inspect({ a: { b: { c: 2 } } }, false, 1),
      '{ a: { b: [Object] } }'
    );
    assert.strictEqual(
      util.inspect({ a: { b: ['c'] } }, false, 1),
      '{ a: { b: [Array] } }'
    );
    assert.strictEqual(util.inspect(new Uint8Array(0)), 'Uint8Array(0) []');
    assert(
      inspect(new Uint8Array(0), { showHidden: true }).includes('[buffer]')
    );
    assert.strictEqual(
      util.inspect(
        Object.create(
          {},
          { visible: { value: 1, enumerable: true }, hidden: { value: 2 } }
        )
      ),
      '{ visible: 1 }'
    );
    assert.strictEqual(
      util.inspect(
        Object.assign(new String('hello'), { [Symbol('foo')]: 123 }),
        { showHidden: true }
      ),
      "[String: 'hello'] { [length]: 5, [Symbol(foo)]: 123 }"
    );

    {
      const regexp = /regexp/;
      regexp.aprop = 42;
      assert.strictEqual(
        util.inspect({ a: regexp }, false, 0),
        '{ a: /regexp/ }'
      );
    }

    assert.match(
      util.inspect({ a: { a: { a: { a: {} } } } }, undefined, undefined, true),
      /Object/
    );
    assert.doesNotMatch(
      util.inspect({ a: { a: { a: { a: {} } } } }, undefined, null, true),
      /Object/
    );

    {
      const showHidden = true;
      const ab = new Uint8Array([1, 2, 3, 4]).buffer;
      const dv = new DataView(ab, 1, 2);
      assert.strictEqual(
        util.inspect(ab, showHidden),
        'ArrayBuffer { [Uint8Contents]: <01 02 03 04>, byteLength: 4 }'
      );
      assert.strictEqual(
        util.inspect(new DataView(ab, 1, 2), showHidden),
        'DataView {\n' +
          '  byteLength: 2,\n' +
          '  byteOffset: 1,\n' +
          '  buffer: ArrayBuffer {' +
          ' [Uint8Contents]: <01 02 03 04>, byteLength: 4 }\n}'
      );
      assert.strictEqual(
        util.inspect(ab, showHidden),
        'ArrayBuffer { [Uint8Contents]: <01 02 03 04>, byteLength: 4 }'
      );
      assert.strictEqual(
        util.inspect(dv, showHidden),
        'DataView {\n' +
          '  byteLength: 2,\n' +
          '  byteOffset: 1,\n' +
          '  buffer: ArrayBuffer { [Uint8Contents]: ' +
          '<01 02 03 04>, byteLength: 4 }\n}'
      );
      ab.x = 42;
      dv.y = 1337;
      assert.strictEqual(
        util.inspect(ab, showHidden),
        'ArrayBuffer { [Uint8Contents]: <01 02 03 04>, ' +
          'byteLength: 4, x: 42 }'
      );
      assert.strictEqual(
        util.inspect(dv, showHidden),
        'DataView {\n' +
          '  byteLength: 2,\n' +
          '  byteOffset: 1,\n' +
          '  buffer: ArrayBuffer { [Uint8Contents]: <01 02 03 04>,' +
          ' byteLength: 4, x: 42 },\n' +
          '  y: 1337\n}'
      );
    }

    {
      const ab = new ArrayBuffer(42);
      assert.strictEqual(ab.byteLength, 42);
      structuredClone(ab, { transfer: [ab] });
      assert.strictEqual(ab.byteLength, 0);
      assert.strictEqual(
        util.inspect(ab),
        'ArrayBuffer { (detached), byteLength: 0 }'
      );
    }

    // Truncate output for ArrayBuffers using plural or singular bytes
    {
      const ab = new ArrayBuffer(3);
      assert.strictEqual(
        util.inspect(ab, { showHidden: true, maxArrayLength: 2 }),
        'ArrayBuffer { [Uint8Contents]' +
          ': <00 00 ... 1 more byte>, byteLength: 3 }'
      );
      assert.strictEqual(
        util.inspect(ab, { showHidden: true, maxArrayLength: 1 }),
        'ArrayBuffer { [Uint8Contents]' +
          ': <00 ... 2 more bytes>, byteLength: 3 }'
      );
    }

    [
      Float32Array,
      Float64Array,
      Int16Array,
      Int32Array,
      Int8Array,
      Uint16Array,
      Uint32Array,
      Uint8Array,
      Uint8ClampedArray,
    ].forEach((constructor) => {
      const length = 2;
      const byteLength = length * constructor.BYTES_PER_ELEMENT;
      const array = new constructor(new ArrayBuffer(byteLength), 0, length);
      array[0] = 65;
      array[1] = 97;
      assert.strictEqual(
        util.inspect(array, { showHidden: true }),
        `${constructor.name}(${length}) [\n` +
          '  65,\n' +
          '  97,\n' +
          `  [BYTES_PER_ELEMENT]: ${constructor.BYTES_PER_ELEMENT},\n` +
          `  [length]: ${length},\n` +
          `  [byteLength]: ${byteLength},\n` +
          '  [byteOffset]: 0,\n' +
          `  [buffer]: ArrayBuffer { byteLength: ${byteLength} }\n]`
      );
      assert.strictEqual(
        util.inspect(array, false),
        `${constructor.name}(${length}) [ 65, 97 ]`
      );
    });

    {
      const brokenLength = new Float32Array(2);
      Object.defineProperty(brokenLength, 'length', { value: -1 });
      assert.strictEqual(inspect(brokenLength), 'Float32Array(2) [ 0n, 0n ]');
    }

    assert.strictEqual(
      util.inspect(
        Object.create(
          {},
          {
            visible: { value: 1, enumerable: true },
            hidden: { value: 2 },
          }
        ),
        { showHidden: true }
      ),
      '{ visible: 1, [hidden]: 2 }'
    );
    // Objects without prototype.
    assert.strictEqual(
      util.inspect(
        Object.create(null, {
          name: { value: 'Tim', enumerable: true },
          hidden: { value: 'secret' },
        }),
        { showHidden: true }
      ),
      "[Object: null prototype] { name: 'Tim', [hidden]: 'secret' }"
    );

    assert.strictEqual(
      util.inspect(
        Object.create(null, {
          name: { value: 'Tim', enumerable: true },
          hidden: { value: 'secret' },
        })
      ),
      "[Object: null prototype] { name: 'Tim' }"
    );

    // Dynamic properties.
    {
      assert.strictEqual(
        util.inspect({
          get readonly() {
            return 1;
          },
        }),
        '{ readonly: [Getter] }'
      );

      assert.strictEqual(
        util.inspect({
          get readwrite() {
            return 1;
          },
          set readwrite(val) {},
        }),
        '{ readwrite: [Getter/Setter] }'
      );

      assert.strictEqual(
        // eslint-disable-next-line accessor-pairs
        util.inspect({ set writeonly(val) {} }),
        '{ writeonly: [Setter] }'
      );

      const value = {};
      value.a = value;
      assert.strictEqual(util.inspect(value), '<ref *1> { a: [Circular *1] }');
      const getterFn = {
        get one() {
          return null;
        },
      };
      assert.strictEqual(
        util.inspect(getterFn, { getters: true }),
        '{ one: [Getter: null] }'
      );
    }

    // Array with dynamic properties.
    {
      const value = [1, 2, 3];
      Object.defineProperty(value, 'growingLength', {
        enumerable: true,
        get: function () {
          this.push(true);
          return this.length;
        },
      });
      Object.defineProperty(value, '-1', {
        enumerable: true,
        value: -1,
      });
      assert.strictEqual(
        util.inspect(value),
        "[ 1, 2, 3, growingLength: [Getter], '-1': -1 ]"
      );
    }

    // Array with inherited number properties.
    {
      class CustomArray extends Array {}
      CustomArray.prototype[5] = 'foo';
      CustomArray.prototype[49] = 'bar';
      CustomArray.prototype.foo = true;
      const arr = new CustomArray(50);
      arr[49] = 'I win';
      assert.strictEqual(
        util.inspect(arr),
        "CustomArray(50) [ <49 empty items>, 'I win' ]"
      );
      assert.strictEqual(
        util.inspect(arr, { showHidden: true }),
        'CustomArray(50) [\n' +
          '  <49 empty items>,\n' +
          "  'I win',\n" +
          '  [length]: 50,\n' +
          "  '5': 'foo',\n" +
          '  foo: true\n' +
          ']'
      );
    }

    // Array with extra properties.
    {
      const arr = [1, 2, 3, ,]; // eslint-disable-line no-sparse-arrays
      arr.foo = 'bar';
      assert.strictEqual(
        util.inspect(arr),
        "[ 1, 2, 3, <1 empty item>, foo: 'bar' ]"
      );

      const arr2 = [];
      assert.strictEqual(
        util.inspect([], { showHidden: true }),
        '[ [length]: 0 ]'
      );
      arr2['00'] = 1;
      assert.strictEqual(util.inspect(arr2), "[ '00': 1 ]");
      assert.strictEqual(
        util.inspect(arr2, { showHidden: true }),
        "[ [length]: 0, '00': 1 ]"
      );
      arr2[1] = 0;
      assert.strictEqual(util.inspect(arr2), "[ <1 empty item>, 0, '00': 1 ]");
      assert.strictEqual(
        util.inspect(arr2, { showHidden: true }),
        "[ <1 empty item>, 0, [length]: 2, '00': 1 ]"
      );
      delete arr2[1];
      assert.strictEqual(util.inspect(arr2), "[ <2 empty items>, '00': 1 ]");
      assert.strictEqual(
        util.inspect(arr2, { showHidden: true }),
        "[ <2 empty items>, [length]: 2, '00': 1 ]"
      );
      arr2['01'] = 2;
      assert.strictEqual(
        util.inspect(arr2),
        "[ <2 empty items>, '00': 1, '01': 2 ]"
      );
      assert.strictEqual(
        util.inspect(arr2, { showHidden: true }),
        "[ <2 empty items>, [length]: 2, '00': 1, '01': 2 ]"
      );
      delete arr2['00'];
      arr2[0] = 0;
      assert.strictEqual(util.inspect(arr2), "[ 0, <1 empty item>, '01': 2 ]");
      assert.strictEqual(
        util.inspect(arr2, { showHidden: true }),
        "[ 0, <1 empty item>, [length]: 2, '01': 2 ]"
      );
      delete arr2['01'];
      arr2[2 ** 32 - 2] = 'max';
      arr2[2 ** 32 - 1] = 'too far';
      assert.strictEqual(
        util.inspect(arr2),
        "[ 0, <4294967293 empty items>, 'max', '4294967295': 'too far' ]"
      );

      const arr3 = [];
      arr3[-1] = -1;
      assert.strictEqual(util.inspect(arr3), "[ '-1': -1 ]");
    }

    // Indices out of bounds.
    {
      const arr = [];
      arr[2 ** 32] = true; // Not a valid array index.
      assert.strictEqual(util.inspect(arr), "[ '4294967296': true ]");
      arr[0] = true;
      arr[10] = true;
      assert.strictEqual(
        util.inspect(arr),
        "[ true, <9 empty items>, true, '4294967296': true ]"
      );
      arr[2 ** 32 - 2] = true;
      arr[2 ** 32 - 1] = true;
      arr[2 ** 32 + 1] = true;
      delete arr[0];
      delete arr[10];
      assert.strictEqual(
        util.inspect(arr),
        [
          '[',
          '<4294967294 empty items>,',
          'true,',
          "'4294967296': true,",
          "'4294967295': true,",
          "'4294967297': true\n]",
        ].join('\n  ')
      );
    }

    // Function with properties.
    {
      const value = () => {};
      value.aprop = 42;
      assert.strictEqual(
        util.inspect(value),
        '[Function: value] { aprop: 42 }'
      );
    }

    // Anonymous function with properties.
    {
      const value = (() => function () {})();
      value.aprop = 42;
      assert.strictEqual(
        util.inspect(value),
        '[Function (anonymous)] { aprop: 42 }'
      );
    }

    // Regular expressions with properties.
    {
      const value = /123/gi;
      value.aprop = 42;
      assert.strictEqual(util.inspect(value), '/123/gi { aprop: 42 }');
    }

    // Dates with properties.
    {
      const value = new Date('Sun, 14 Feb 2010 11:48:40 GMT');
      value.aprop = 42;
      assert.strictEqual(
        util.inspect(value),
        '2010-02-14T11:48:40.000Z { aprop: 42 }'
      );
    }

    // Test positive/negative zero.
    assert.strictEqual(util.inspect(0), '0');
    assert.strictEqual(util.inspect(-0), '-0');
    // Edge case from check.
    assert.strictEqual(util.inspect(-5e-324), '-5e-324');

    // Test for sparse array.
    {
      const a = ['foo', 'bar', 'baz'];
      assert.strictEqual(util.inspect(a), "[ 'foo', 'bar', 'baz' ]");
      delete a[1];
      assert.strictEqual(util.inspect(a), "[ 'foo', <1 empty item>, 'baz' ]");
      assert.strictEqual(
        util.inspect(a, true),
        "[ 'foo', <1 empty item>, 'baz', [length]: 3 ]"
      );
      assert.strictEqual(util.inspect(new Array(5)), '[ <5 empty items> ]');
      a[3] = 'bar';
      a[100] = 'qux';
      assert.strictEqual(
        util.inspect(a, { breakLength: Infinity }),
        "[ 'foo', <1 empty item>, 'baz', 'bar', <96 empty items>, 'qux' ]"
      );
      delete a[3];
      assert.strictEqual(
        util.inspect(a, { maxArrayLength: 4 }),
        "[ 'foo', <1 empty item>, 'baz', <97 empty items>, ... 1 more item ]"
      );
      // test 4 special case
      assert.strictEqual(
        util.inspect(a, {
          maxArrayLength: 2,
        }),
        "[ 'foo', <1 empty item>, ... 99 more items ]"
      );
    }

    // Test for property descriptors.
    {
      const getter = Object.create(null, {
        a: {
          get: function () {
            return 'aaa';
          },
        },
      });
      const setter = Object.create(null, {
        b: {
          // eslint-disable-line accessor-pairs
          set: function () {},
        },
      });
      const getterAndSetter = Object.create(null, {
        c: {
          get: function () {
            return 'ccc';
          },
          set: function () {},
        },
      });
      assert.strictEqual(
        util.inspect(getter, true),
        '[Object: null prototype] { [a]: [Getter] }'
      );
      assert.strictEqual(
        util.inspect(setter, true),
        '[Object: null prototype] { [b]: [Setter] }'
      );
      assert.strictEqual(
        util.inspect(getterAndSetter, true),
        '[Object: null prototype] { [c]: [Getter/Setter] }'
      );
    }

    // Exceptions should print the error message, not '{}'.
    {
      [
        new Error(),
        new Error('FAIL'),
        new TypeError('FAIL'),
        new SyntaxError('FAIL'),
      ].forEach((err) => {
        assert.strictEqual(util.inspect(err), err.stack);
      });
      assert.throws(
        () => undef(), // eslint-disable-line no-undef
        (e) => {
          assert.strictEqual(util.inspect(e), e.stack);
          return true;
        }
      );

      const ex = util.inspect(new Error('FAIL'), true);
      assert(ex.includes('Error: FAIL'));
      assert(ex.includes('[stack]'));
      assert(ex.includes('[message]'));
    }

    {
      const falsyCause1 = new Error('', { cause: false });
      delete falsyCause1.stack;
      const falsyCause2 = new Error(undefined, { cause: null });
      falsyCause2.stack = '';
      const undefinedCause = new Error('', { cause: undefined });
      undefinedCause.stack = '';

      assert.strictEqual(
        util.inspect(falsyCause1),
        '[Error] { [cause]: false }'
      );
      assert.strictEqual(
        util.inspect(falsyCause2),
        '[Error] { [cause]: null }'
      );
      assert.strictEqual(
        util.inspect(undefinedCause),
        '[Error] { [cause]: undefined }'
      );
    }

    {
      const tmp = Error.stackTraceLimit;
      Error.stackTraceLimit = 0;
      const err = new Error('foo');
      const err2 = new Error('foo\nbar');
      assert.strictEqual(util.inspect(err, { compact: true }), '[Error: foo]');
      assert(err.stack);
      delete err.stack;
      assert(!err.stack);
      assert.strictEqual(util.inspect(err, { compact: true }), '[Error: foo]');
      assert.strictEqual(
        util.inspect(err2, { compact: true }),
        '[Error: foo\nbar]'
      );

      err.bar = true;
      err2.bar = true;

      assert.strictEqual(
        util.inspect(err, { compact: true }),
        '{ [Error: foo] bar: true }'
      );
      assert.strictEqual(
        util.inspect(err2, { compact: true }),
        '{ [Error: foo\nbar]\n  bar: true }'
      );
      assert.strictEqual(
        util.inspect(err, { compact: true, breakLength: 5 }),
        '{ [Error: foo]\n  bar: true }'
      );
      assert.strictEqual(
        util.inspect(err, { compact: true, breakLength: 1 }),
        '{ [Error: foo]\n  bar:\n   true }'
      );
      assert.strictEqual(
        util.inspect(err2, { compact: true, breakLength: 5 }),
        '{ [Error: foo\nbar]\n  bar: true }'
      );
      assert.strictEqual(
        util.inspect(err, { compact: false }),
        '[Error: foo] {\n  bar: true\n}'
      );
      assert.strictEqual(
        util.inspect(err2, { compact: false }),
        '[Error: foo\nbar] {\n  bar: true\n}'
      );

      Error.stackTraceLimit = tmp;
    }

    // Prevent enumerable error properties from being printed.
    {
      let err = new Error('foobar');
      err.message = 'foobar';
      let out = util.inspect(err).split('\n');
      assert.strictEqual(out[0], 'Error: foobar');
      assert(out[1].startsWith('    at '));
      // Reset the error, the stack is otherwise not recreated.
      err = new Error();
      err.message = 'foobar';
      out = util.inspect(err).split('\n');
      assert.strictEqual(out[0], 'Error');
      assert(out[1].startsWith('    at '));
      assert.strictEqual(out[out.length - 2], "  message: 'foobar'");
      assert.strictEqual(out[out.length - 1], '}');
      // Reset the error, the stack is otherwise not recreated.
      err = new Error('foobar');
      err.name = 'Unique';
      Object.defineProperty(err, 'stack', {
        value: err.stack,
        enumerable: true,
      });
      out = util.inspect(err).split('\n');
      assert.strictEqual(out[0], 'Unique: foobar');
      assert(out[1].startsWith('    at '));
      err.name = 'Baz';
      out = util.inspect(err).split('\n');
      assert.strictEqual(out[0], 'Unique: foobar');
      assert.strictEqual(out[out.length - 2], "  name: 'Baz'");
      assert.strictEqual(out[out.length - 1], '}');
    }

    // Doesn't capture stack trace.
    {
      function BadCustomError(msg) {
        Error.call(this);
        Object.defineProperty(this, 'message', {
          value: msg,
          enumerable: false,
        });
        Object.defineProperty(this, 'name', {
          value: 'BadCustomError',
          enumerable: false,
        });
      }
      Object.setPrototypeOf(BadCustomError.prototype, Error.prototype);
      Object.setPrototypeOf(BadCustomError, Error);
      assert.strictEqual(
        util.inspect(new BadCustomError('foo')),
        '[BadCustomError: foo]'
      );
    }

    // Tampered error stack or name property (different type than string).
    // Note: Symbols are not supported by `Error#toString()` which is called by
    // accessing the `stack` property.
    [
      [404, '404: foo', '[404]'],
      [0, '0: foo', '[RangeError: foo]'],
      [0n, '0: foo', '[RangeError: foo]'],
      [null, 'null: foo', '[RangeError: foo]'],
      [undefined, 'RangeError: foo', '[RangeError: foo]'],
      [false, 'false: foo', '[RangeError: foo]'],
      ['', 'foo', '[RangeError: foo]'],
      [[1, 2, 3], '1,2,3: foo', '[1,2,3]'],
    ].forEach(([value, outputStart, stack]) => {
      let err = new RangeError('foo');
      err.name = value;
      assert(
        util.inspect(err).startsWith(outputStart),
        util.format(
          'The name set to %o did not result in the expected output "%s"',
          value,
          outputStart
        )
      );

      err = new RangeError('foo');
      err.stack = value;
      assert.strictEqual(util.inspect(err), stack);
    });

    // https://github.com/nodejs/node-v0.x-archive/issues/1941
    assert.strictEqual(util.inspect({ __proto__: Date.prototype }), 'Date {}');

    // https://github.com/nodejs/node-v0.x-archive/issues/1944
    {
      const d = new Date();
      d.toUTCString = null;
      util.inspect(d);
    }

    // Should not throw.
    {
      const d = new Date();
      d.toISOString = null;
      util.inspect(d);
    }

    // Should not throw.
    {
      const r = /regexp/;
      r.toString = null;
      util.inspect(r);
    }

    // See https://github.com/nodejs/node-v0.x-archive/issues/2225
    {
      const x = { [util.inspect.custom]: util.inspect };
      assert(
        util
          .inspect(x)
          .includes(
            '[Symbol(nodejs.util.inspect.custom)]: [Function: inspect] {\n'
          )
      );
    }

    // `util.inspect` should display the escaped value of a key.
    {
      const w = {
        '\\': 1,
        '\\\\': 2,
        '\\\\\\': 3,
        '\\\\\\\\': 4,
        '\n': 5,
        '\r': 6,
      };

      const y = ['a', 'b', 'c'];
      y['\\\\'] = 'd';
      y['\n'] = 'e';
      y['\r'] = 'f';

      assert.strictEqual(
        util.inspect(w),
        "{ '\\\\': 1, '\\\\\\\\': 2, '\\\\\\\\\\\\': 3, " +
          "'\\\\\\\\\\\\\\\\': 4, '\\n': 5, '\\r': 6 }"
      );
      assert.strictEqual(
        util.inspect(y),
        "[ 'a', 'b', 'c', '\\\\\\\\': 'd', " + "'\\n': 'e', '\\r': 'f' ]"
      );
    }

    // Escape unpaired surrogate pairs.
    {
      const edgeChar = String.fromCharCode(0xd799);

      for (let charCode = 0xd800; charCode < 0xdfff; charCode++) {
        const surrogate = String.fromCharCode(charCode);

        assert.strictEqual(
          util.inspect(surrogate),
          `'\\u${charCode.toString(16)}'`
        );
        assert.strictEqual(
          util.inspect(`${'a'.repeat(200)}${surrogate}`),
          `'${'a'.repeat(200)}\\u${charCode.toString(16)}'`
        );
        assert.strictEqual(
          util.inspect(`${surrogate}${'a'.repeat(200)}`),
          `'\\u${charCode.toString(16)}${'a'.repeat(200)}'`
        );
        if (charCode < 0xdc00) {
          const highSurrogate = surrogate;
          const lowSurrogate = String.fromCharCode(charCode + 1024);
          assert(
            !util
              .inspect(`${edgeChar}${highSurrogate}${lowSurrogate}${edgeChar}`)
              .includes('\\u')
          );
          assert.strictEqual(
            (
              util
                .inspect(`${highSurrogate}${highSurrogate}${lowSurrogate}`)
                .match(/\\u/g) ?? []
            ).length,
            1
          );
        } else {
          assert.strictEqual(
            util.inspect(`${edgeChar}${surrogate}${edgeChar}`),
            `'${edgeChar}\\u${charCode.toString(16)}${edgeChar}'`
          );
        }
      }
    }

    // Test util.inspect.styles and util.inspect.colors.
    {
      function testColorStyle(style, input) {
        const colorName = util.inspect.styles[style];
        let color = ['', ''];
        if (util.inspect.colors[colorName])
          color = util.inspect.colors[colorName];

        const withoutColor = util.inspect(input, false, 0, false);
        const withColor = util.inspect(input, false, 0, true);
        const expect = `\u001b[${color[0]}m${withoutColor}\u001b[${color[1]}m`;
        assert.strictEqual(
          withColor,
          expect,
          `util.inspect color for style ${style}`
        );
      }

      testColorStyle('special', function () {});
      testColorStyle('number', 123.456);
      testColorStyle('boolean', true);
      testColorStyle('undefined', undefined);
      testColorStyle('null', null);
      testColorStyle('string', 'test string');
      testColorStyle('date', new Date());
      testColorStyle('regexp', /regexp/);
    }

    // An object with "hasOwnProperty" overwritten should not throw.
    util.inspect({ hasOwnProperty: null });

    // New API, accepts an "options" object.
    {
      const subject = { foo: 'bar', hello: 31, a: { b: { c: { d: 0 } } } };
      Object.defineProperty(subject, 'hidden', {
        enumerable: false,
        value: null,
      });

      assert.strictEqual(
        util.inspect(subject, { showHidden: false }).includes('hidden'),
        false
      );
      assert.strictEqual(
        util.inspect(subject, { showHidden: true }).includes('hidden'),
        true
      );
      assert.strictEqual(
        util.inspect(subject, { colors: false }).includes('\u001b[32m'),
        false
      );
      assert.strictEqual(
        util.inspect(subject, { colors: true }).includes('\u001b[32m'),
        true
      );
      assert.strictEqual(
        util.inspect(subject, { depth: 2 }).includes('c: [Object]'),
        true
      );
      assert.strictEqual(
        util.inspect(subject, { depth: 0 }).includes('a: [Object]'),
        true
      );
      assert.strictEqual(
        util.inspect(subject, { depth: null }).includes('{ d: 0 }'),
        true
      );
      assert.strictEqual(
        util.inspect(subject, { depth: undefined }).includes('{ d: 0 }'),
        true
      );
    }

    {
      // "customInspect" option can enable/disable calling [util.inspect.custom]().
      const subject = { [util.inspect.custom]: () => 123 };

      assert.strictEqual(
        util.inspect(subject, { customInspect: true }).includes('123'),
        true
      );
      assert.strictEqual(
        util.inspect(subject, { customInspect: true }).includes('inspect'),
        false
      );
      assert.strictEqual(
        util.inspect(subject, { customInspect: false }).includes('123'),
        false
      );
      assert.strictEqual(
        util.inspect(subject, { customInspect: false }).includes('inspect'),
        true
      );

      // A custom [util.inspect.custom]() should be able to return other Objects.
      subject[util.inspect.custom] = () => ({ foo: 'bar' });

      assert.strictEqual(util.inspect(subject), "{ foo: 'bar' }");

      subject[util.inspect.custom] = commonMustCall((depth, opts, inspect) => {
        const clone = { ...opts };
        // This might change at some point but for now we keep the stylize function.
        // The function should either be documented or an alternative should be
        // implemented.
        assert.strictEqual(typeof opts.stylize, 'function');
        assert.strictEqual(opts.seen, undefined);
        assert.strictEqual(opts.budget, undefined);
        assert.strictEqual(opts.indentationLvl, undefined);
        assert.strictEqual(opts.showHidden, false);
        assert.strictEqual(inspect, util.inspect);
        assert.deepStrictEqual(
          new Set(Object.keys(inspect.defaultOptions).concat(['stylize'])),
          new Set(Object.keys(opts))
        );
        opts.showHidden = true;
        return {
          [inspect.custom]: commonMustCall((depth, opts2) => {
            assert.deepStrictEqual(clone, opts2);
          }),
        };
      });

      util.inspect(subject);

      // util.inspect.custom is a shared symbol which can be accessed as
      // Symbol.for("nodejs.util.inspect.custom").
      const inspect = Symbol.for('nodejs.util.inspect.custom');

      subject[inspect] = () => ({ baz: 'quux' });

      assert.strictEqual(util.inspect(subject), "{ baz: 'quux' }");

      subject[inspect] = (depth, opts) => {
        assert.strictEqual(opts.customInspectOptions, true);
        assert.strictEqual(opts.seen, null);
        return {};
      };

      util.inspect(subject, { customInspectOptions: true, seen: null });
    }

    {
      const subject = {
        [util.inspect.custom]: commonMustCall((depth, opts) => {
          assert.strictEqual(depth, null);
          assert.strictEqual(opts.compact, true);
        }),
      };
      util.inspect(subject, { depth: null, compact: true });
    }

    {
      // Returning `this` from a custom inspection function works.
      const subject = {
        a: 123,
        [util.inspect.custom]() {
          return this;
        },
      };
      const UIC = 'nodejs.util.inspect.custom';
      assert.strictEqual(
        util.inspect(subject),
        `{\n  a: 123,\n  [Symbol(${UIC})]: [Function: [${UIC}]]\n}`
      );
    }

    // Verify that it's possible to use the stylize function to manipulate input.
    assert.strictEqual(
      util.inspect([1, 2, 3], {
        stylize() {
          return 'x';
        },
      }),
      '[ x, x, x ]'
    );

    // Using `util.inspect` with "colors" option should produce as many lines as
    // without it.
    {
      function testLines(input) {
        const countLines = (str) => (str.match(/\n/g) || []).length;
        const withoutColor = util.inspect(input);
        const withColor = util.inspect(input, { colors: true });
        assert.strictEqual(countLines(withoutColor), countLines(withColor));
      }

      const bigArray = new Array(100).fill().map((value, index) => index);

      testLines([1, 2, 3, 4, 5, 6, 7]);
      testLines(bigArray);
      testLines({ foo: 'bar', baz: 35, b: { a: 35 } });
      testLines({
        a: { a: 3, b: 1, c: 1, d: 1, e: 1, f: 1, g: 1, h: 1 },
        b: 1,
      });
      testLines({
        foo: 'bar',
        baz: 35,
        b: { a: 35 },
        veryLongKey: 'very long value',
        evenLongerKey: ['with even longer value in array'],
      });
    }

    // Test boxed primitives output the correct values.
    assert.strictEqual(util.inspect(new String('test')), "[String: 'test']");
    assert.strictEqual(
      util.inspect(new String('test'), { colors: true }),
      "\u001b[32m[String: 'test']\u001b[39m"
    );
    assert.strictEqual(
      util.inspect(Object(Symbol('test'))),
      '[Symbol: Symbol(test)]'
    );
    assert.strictEqual(util.inspect(new Boolean(false)), '[Boolean: false]');
    assert.strictEqual(
      util.inspect(Object.setPrototypeOf(new Boolean(true), null)),
      '[Boolean (null prototype): true]'
    );
    assert.strictEqual(util.inspect(new Number(0)), '[Number: 0]');
    assert.strictEqual(
      util.inspect(
        Object.defineProperty(
          Object.setPrototypeOf(new Number(-0), Array.prototype),
          Symbol.toStringTag,
          { value: 'Foobar' }
        )
      ),
      '[Number (Array): -0] [Foobar]'
    );
    assert.strictEqual(util.inspect(new Number(-1.1)), '[Number: -1.1]');
    assert.strictEqual(util.inspect(new Number(13.37)), '[Number: 13.37]');

    // Test boxed primitives with own properties.
    {
      const str = new String('baz');
      str.foo = 'bar';
      assert.strictEqual(util.inspect(str), "[String: 'baz'] { foo: 'bar' }");

      const bool = new Boolean(true);
      bool.foo = 'bar';
      assert.strictEqual(util.inspect(bool), "[Boolean: true] { foo: 'bar' }");

      const num = new Number(13.37);
      num.foo = 'bar';
      assert.strictEqual(util.inspect(num), "[Number: 13.37] { foo: 'bar' }");

      const sym = Object(Symbol('foo'));
      sym.foo = 'bar';
      assert.strictEqual(
        util.inspect(sym),
        "[Symbol: Symbol(foo)] { foo: 'bar' }"
      );

      const big = Object(BigInt(55));
      big.foo = 'bar';
      assert.strictEqual(util.inspect(big), "[BigInt: 55n] { foo: 'bar' }");
    }

    // Test es6 Symbol.
    if (typeof Symbol !== 'undefined') {
      assert.strictEqual(util.inspect(Symbol()), 'Symbol()');
      assert.strictEqual(util.inspect(Symbol(123)), 'Symbol(123)');
      assert.strictEqual(util.inspect(Symbol('hi')), 'Symbol(hi)');
      assert.strictEqual(util.inspect([Symbol()]), '[ Symbol() ]');
      assert.strictEqual(util.inspect({ foo: Symbol() }), '{ foo: Symbol() }');

      const options = { showHidden: true };
      let subject = {};

      subject[Symbol('sym\nbol')] = 42;

      assert.strictEqual(util.inspect(subject), '{ [Symbol(sym\\nbol)]: 42 }');
      assert.strictEqual(
        util.inspect(subject, options),
        '{ [Symbol(sym\\nbol)]: 42 }'
      );

      Object.defineProperty(subject, Symbol(), {
        enumerable: false,
        value: 'non-enum',
      });
      assert.strictEqual(util.inspect(subject), '{ [Symbol(sym\\nbol)]: 42 }');
      assert.strictEqual(
        util.inspect(subject, options),
        "{ [Symbol(sym\\nbol)]: 42, [Symbol()]: 'non-enum' }"
      );

      subject = [1, 2, 3];
      subject[Symbol('symbol')] = 42;

      assert.strictEqual(
        util.inspect(subject),
        '[ 1, 2, 3, [Symbol(symbol)]: 42 ]'
      );
    }

    // Test Set.
    {
      assert.strictEqual(util.inspect(new Set()), 'Set(0) {}');
      assert.strictEqual(
        util.inspect(new Set([1, 2, 3])),
        'Set(3) { 1, 2, 3 }'
      );
      assert.strictEqual(
        util.inspect(new Set([1, 2, 3]), { maxArrayLength: 1 }),
        'Set(3) { 1, ... 2 more items }'
      );
      const set = new Set(['foo']);
      set.bar = 42;
      assert.strictEqual(
        util.inspect(set, { showHidden: true }),
        "Set(1) { 'foo', bar: 42 }"
      );
    }

    // Test circular Set.
    {
      const set = new Set();
      set.add(set);
      assert.strictEqual(
        util.inspect(set),
        '<ref *1> Set(1) { [Circular *1] }'
      );
    }

    // Test Map.
    {
      assert.strictEqual(util.inspect(new Map()), 'Map(0) {}');
      assert.strictEqual(
        util.inspect(
          new Map([
            [1, 'a'],
            [2, 'b'],
            [3, 'c'],
          ])
        ),
        "Map(3) { 1 => 'a', 2 => 'b', 3 => 'c' }"
      );
      assert.strictEqual(
        util.inspect(
          new Map([
            [1, 'a'],
            [2, 'b'],
            [3, 'c'],
          ]),
          { maxArrayLength: 1 }
        ),
        "Map(3) { 1 => 'a', ... 2 more items }"
      );
      const map = new Map([['foo', null]]);
      map.bar = 42;
      assert.strictEqual(
        util.inspect(map, true),
        "Map(1) { 'foo' => null, bar: 42 }"
      );
    }

    // Test circular Map.
    {
      const map = new Map();
      map.set(map, 'map');
      assert.strictEqual(
        inspect(map),
        "<ref *1> Map(1) { [Circular *1] => 'map' }"
      );
      map.set(map, map);
      assert.strictEqual(
        inspect(map),
        '<ref *1> Map(1) { [Circular *1] => [Circular *1] }'
      );
      map.delete(map);
      map.set('map', map);
      assert.strictEqual(
        inspect(map),
        "<ref *1> Map(1) { 'map' => [Circular *1] }"
      );
    }

    // Test multiple circular references.
    {
      const obj = {};
      obj.a = [obj];
      obj.b = {};
      obj.b.inner = obj.b;
      obj.b.obj = obj;

      assert.strictEqual(
        inspect(obj),
        '<ref *1> {\n' +
          '  a: [ [Circular *1] ],\n' +
          '  b: <ref *2> { inner: [Circular *2], obj: [Circular *1] }\n' +
          '}'
      );
    }

    // Test Promise.
    {
      const resolved = Promise.resolve(3);
      assert.strictEqual(util.inspect(resolved), 'Promise { 3 }');

      const rejected = Promise.reject(3);
      assert.strictEqual(util.inspect(rejected), 'Promise { <rejected> 3 }');
      // Squelch UnhandledPromiseRejection.
      rejected.catch(() => {});

      const pending = new Promise(() => {});
      assert.strictEqual(util.inspect(pending), 'Promise { <pending> }');

      const promiseWithProperty = Promise.resolve('foo');
      promiseWithProperty.bar = 42;
      assert.strictEqual(
        util.inspect(promiseWithProperty),
        "Promise { 'foo', bar: 42 }"
      );
    }

    // Make sure it doesn't choke on polyfills. Unlike Set/Map, there is no standard
    // interface to synchronously inspect a Promise, so our techniques only work on
    // a bonafide native Promise.
    {
      const oldPromise = Promise;
      globalThis.Promise = function () {
        this.bar = 42;
      };
      assert.strictEqual(util.inspect(new Promise()), '{ bar: 42 }');
      globalThis.Promise = oldPromise;
    }

    // Test Map iterators.
    {
      const map = new Map([['foo', 'bar']]);
      assert.strictEqual(util.inspect(map.keys()), "[Map Iterator] { 'foo' }");
      const mapValues = map.values();
      Object.defineProperty(mapValues, Symbol.toStringTag, { value: 'Foo' });
      assert.strictEqual(
        util.inspect(mapValues),
        "[Foo] [Map Iterator] { 'bar' }"
      );
      map.set('A', 'B!');
      assert.strictEqual(
        util.inspect(map.entries(), { maxArrayLength: 1 }),
        "[Map Entries] { [ 'foo', 'bar' ], ... 1 more item }"
      );
      // Make sure the iterator doesn't get consumed.
      const keys = map.keys();
      assert.strictEqual(util.inspect(keys), "[Map Iterator] { 'foo', 'A' }");
      assert.strictEqual(util.inspect(keys), "[Map Iterator] { 'foo', 'A' }");
      keys.extra = true;
      assert.strictEqual(
        util.inspect(keys, { maxArrayLength: 0 }),
        '[Map Iterator] { ... 2 more items, extra: true }'
      );
    }

    // Test Set iterators.
    {
      const aSet = new Set([1]);
      assert.strictEqual(
        util.inspect(aSet.entries(), { compact: false }),
        '[Set Entries] {\n  [\n    1,\n    1\n  ]\n}'
      );
      aSet.add(3);
      assert.strictEqual(util.inspect(aSet.keys()), '[Set Iterator] { 1, 3 }');
      assert.strictEqual(
        util.inspect(aSet.values()),
        '[Set Iterator] { 1, 3 }'
      );
      const setEntries = aSet.entries();
      Object.defineProperty(setEntries, Symbol.toStringTag, { value: 'Foo' });
      assert.strictEqual(
        util.inspect(setEntries),
        '[Foo] [Set Entries] { [ 1, 1 ], [ 3, 3 ] }'
      );
      // Make sure the iterator doesn't get consumed.
      const keys = aSet.keys();
      Object.defineProperty(keys, Symbol.toStringTag, { value: null });
      assert.strictEqual(util.inspect(keys), '[Set Iterator] { 1, 3 }');
      assert.strictEqual(util.inspect(keys), '[Set Iterator] { 1, 3 }');
      keys.extra = true;
      assert.strictEqual(
        util.inspect(keys, { maxArrayLength: 1 }),
        '[Set Iterator] { 1, ... 1 more item, extra: true }'
      );
    }

    // Minimal inspection should still return as much information as possible about
    // the constructor and Symbol.toStringTag.
    {
      class Foo {
        get [Symbol.toStringTag]() {
          return 'ABC';
        }
      }
      const a = new Foo();
      assert.strictEqual(inspect(a, { depth: -1 }), 'Foo [ABC] {}');
      a.foo = true;
      assert.strictEqual(inspect(a, { depth: -1 }), '[Foo [ABC]]');
      Object.defineProperty(a, Symbol.toStringTag, {
        value: 'Foo',
        configurable: true,
        writable: true,
      });
      assert.strictEqual(inspect(a, { depth: -1 }), '[Foo]');
      delete a[Symbol.toStringTag];
      Object.setPrototypeOf(a, null);
      assert.strictEqual(inspect(a, { depth: -1 }), '[Foo: null prototype]');
      delete a.foo;
      assert.strictEqual(inspect(a, { depth: -1 }), '[Foo: null prototype] {}');
      Object.defineProperty(a, Symbol.toStringTag, {
        value: 'ABC',
        configurable: true,
      });
      assert.strictEqual(
        inspect(a, { depth: -1 }),
        '[Foo: null prototype] [ABC] {}'
      );
      Object.defineProperty(a, Symbol.toStringTag, {
        value: 'Foo',
        configurable: true,
      });
      assert.strictEqual(
        inspect(a, { depth: -1 }),
        '[Object: null prototype] [Foo] {}'
      );
    }

    // Test alignment of items in container.
    // Assumes that the first numeric character is the start of an item.
    {
      function checkAlignment(container, start, lineX, end) {
        const lines = util.inspect(container).split('\n');
        lines.forEach((line, i) => {
          if (i === 0) {
            assert.strictEqual(line, start);
          } else if (i === lines.length - 1) {
            assert.strictEqual(line, end);
          } else {
            let expected = lineX.replace('X', i - 1);
            if (i !== lines.length - 2) expected += ',';
            assert.strictEqual(line, expected);
          }
        });
      }

      const bigArray = [];
      for (let i = 0; i < 100; i++) {
        bigArray.push(i);
      }

      const obj = {};
      bigArray.forEach((prop) => {
        obj[prop] = null;
      });

      checkAlignment(obj, '{', "  'X': null", '}');
      checkAlignment(new Set(bigArray), 'Set(100) {', '  X', '}');
      checkAlignment(
        new Map(bigArray.map((number) => [number, null])),
        'Map(100) {',
        '  X => null',
        '}'
      );
    }

    // Test display of constructors.
    {
      class ObjectSubclass {}
      class ArraySubclass extends Array {}
      class SetSubclass extends Set {}
      class MapSubclass extends Map {}
      class PromiseSubclass extends Promise {}
      class SymbolNameClass {
        static name = Symbol('name');
      }

      const x = new ObjectSubclass();
      x.foo = 42;
      assert.strictEqual(util.inspect(x), 'ObjectSubclass { foo: 42 }');
      assert.strictEqual(
        util.inspect(new ArraySubclass(1, 2, 3)),
        'ArraySubclass(3) [ 1, 2, 3 ]'
      );
      assert.strictEqual(
        util.inspect(new SetSubclass([1, 2, 3])),
        'SetSubclass(3) [Set] { 1, 2, 3 }'
      );
      assert.strictEqual(
        util.inspect(new MapSubclass([['foo', 42]])),
        "MapSubclass(1) [Map] { 'foo' => 42 }"
      );
      assert.strictEqual(
        util.inspect(new PromiseSubclass(() => {})),
        'PromiseSubclass [Promise] { <pending> }'
      );
      assert.strictEqual(
        util.inspect(new SymbolNameClass()),
        'Symbol(name) {}'
      );
      assert.strictEqual(
        util.inspect(
          { a: { b: new ArraySubclass([1, [2], 3]) } },
          { depth: 1 }
        ),
        '{ a: { b: [ArraySubclass] } }'
      );
      assert.strictEqual(
        util.inspect(Object.setPrototypeOf(x, null)),
        '[ObjectSubclass: null prototype] { foo: 42 }'
      );
    }

    // Empty and circular before depth.
    {
      const arr = [[[[]]]];
      assert.strictEqual(util.inspect(arr), '[ [ [ [] ] ] ]');
      arr[0][0][0][0] = [];
      assert.strictEqual(util.inspect(arr), '[ [ [ [Array] ] ] ]');
      arr[0][0][0] = {};
      assert.strictEqual(util.inspect(arr), '[ [ [ {} ] ] ]');
      arr[0][0][0] = { a: 2 };
      assert.strictEqual(util.inspect(arr), '[ [ [ [Object] ] ] ]');
      arr[0][0][0] = arr;
      assert.strictEqual(
        util.inspect(arr),
        '<ref *1> [ [ [ [Circular *1] ] ] ]'
      );
      arr[0][0][0] = arr[0][0];
      assert.strictEqual(
        util.inspect(arr),
        '[ [ <ref *1> [ [Circular *1] ] ] ]'
      );
    }

    // Corner cases.
    {
      const x = { constructor: 42 };
      assert.strictEqual(util.inspect(x), '{ constructor: 42 }');
    }

    {
      const x = {};
      Object.defineProperty(x, 'constructor', {
        get: function () {
          throw new Error('should not access constructor');
        },
        enumerable: true,
      });
      assert.strictEqual(util.inspect(x), '{ constructor: [Getter] }');
    }

    {
      const x = new (function () {})(); // eslint-disable-line new-parens
      assert.strictEqual(util.inspect(x), '{}');
    }

    {
      const x = { __proto__: null };
      assert.strictEqual(util.inspect(x), '[Object: null prototype] {}');
    }

    {
      const x = [];
      x[''] = 1;
      assert.strictEqual(util.inspect(x), "[ '': 1 ]");
    }

    // The following maxArrayLength tests were introduced after v6.0.0 was released.
    // Do not backport to v5/v4 unless all of
    // https://github.com/nodejs/node/pull/6334 is backported.
    {
      const x = new Array(101).fill();
      assert(util.inspect(x).endsWith('1 more item\n]'));
      assert(
        !util.inspect(x, { maxArrayLength: 101 }).endsWith('1 more item\n]')
      );
      assert.strictEqual(
        util.inspect(x, { maxArrayLength: -1 }),
        '[ ... 101 more items ]'
      );
      assert.strictEqual(
        util.inspect(x, { maxArrayLength: 0 }),
        '[ ... 101 more items ]'
      );
    }

    {
      const x = Array(101);
      assert.strictEqual(
        util.inspect(x, { maxArrayLength: 0 }),
        '[ ... 101 more items ]'
      );
      assert(
        !util.inspect(x, { maxArrayLength: null }).endsWith('1 more item\n]')
      );
      assert(
        !util.inspect(x, { maxArrayLength: Infinity }).endsWith('1 more item ]')
      );
    }

    {
      const x = new Uint8Array(101);
      assert(util.inspect(x).endsWith('1 more item\n]'));
      assert(!util.inspect(x, { maxArrayLength: 101 }).includes('1 more item'));
      assert.strictEqual(
        util.inspect(x, { maxArrayLength: 0 }),
        'Uint8Array(101) [ ... 101 more items ]'
      );
      assert(
        !util.inspect(x, { maxArrayLength: null }).includes('1 more item')
      );
      assert(
        util.inspect(x, { maxArrayLength: Infinity }).endsWith(' 0, 0\n]')
      );
    }

    {
      const obj = { foo: 'abc', bar: 'xyz' };
      const oneLine = util.inspect(obj, { breakLength: Infinity });
      // Subtract four for the object's two curly braces and two spaces of padding.
      // Add one more to satisfy the strictly greater than condition in the code.
      const breakpoint = oneLine.length - 5;
      const twoLines = util.inspect(obj, { breakLength: breakpoint });

      assert.strictEqual(oneLine, "{ foo: 'abc', bar: 'xyz' }");
      assert.strictEqual(
        util.inspect(obj, { breakLength: breakpoint + 1 }),
        twoLines
      );
      assert.strictEqual(twoLines, "{\n  foo: 'abc',\n  bar: 'xyz'\n}");
    }

    // util.inspect.defaultOptions tests.
    {
      const arr = new Array(101).fill();
      const obj = { a: { a: { a: { a: 1 } } } };

      const oldOptions = { ...util.inspect.defaultOptions };

      // Set single option through property assignment.
      util.inspect.defaultOptions.maxArrayLength = null;
      assert.doesNotMatch(util.inspect(arr), /1 more item/);
      util.inspect.defaultOptions.maxArrayLength = oldOptions.maxArrayLength;
      assert.match(util.inspect(arr), /1 more item/);
      util.inspect.defaultOptions.depth = null;
      assert.doesNotMatch(util.inspect(obj), /Object/);
      util.inspect.defaultOptions.depth = oldOptions.depth;
      assert.match(util.inspect(obj), /Object/);
      assert.strictEqual(
        JSON.stringify(util.inspect.defaultOptions),
        JSON.stringify(oldOptions)
      );

      // Set multiple options through object assignment.
      util.inspect.defaultOptions = { maxArrayLength: null, depth: 2 };
      assert.doesNotMatch(util.inspect(arr), /1 more item/);
      assert.match(util.inspect(obj), /Object/);
      util.inspect.defaultOptions = oldOptions;
      assert.match(util.inspect(arr), /1 more item/);
      assert.match(util.inspect(obj), /Object/);
      assert.strictEqual(
        JSON.stringify(util.inspect.defaultOptions),
        JSON.stringify(oldOptions)
      );

      assert.throws(
        () => {
          util.inspect.defaultOptions = null;
        },
        {
          code: 'ERR_INVALID_ARG_TYPE',
          name: 'TypeError',
          message:
            'The "options" argument must be of type object. ' + 'Received null',
        }
      );

      assert.throws(
        () => {
          util.inspect.defaultOptions = 'bad';
        },
        {
          code: 'ERR_INVALID_ARG_TYPE',
          name: 'TypeError',
          message:
            'The "options" argument must be of type object. ' +
            "Received type string ('bad')",
        }
      );
    }

    // Setting custom inspect property to a non-function should do nothing.
    {
      const obj = { [util.inspect.custom]: 'fhqwhgads' };
      assert.strictEqual(
        util.inspect(obj),
        "{ [Symbol(nodejs.util.inspect.custom)]: 'fhqwhgads' }"
      );
    }

    {
      // @@toStringTag
      const obj = { [Symbol.toStringTag]: 'a' };
      assert.strictEqual(
        util.inspect(obj),
        "{ [Symbol(Symbol.toStringTag)]: 'a' }"
      );
      Object.defineProperty(obj, Symbol.toStringTag, {
        value: 'a',
        enumerable: false,
      });
      assert.strictEqual(util.inspect(obj), 'Object [a] {}');
      assert.strictEqual(
        util.inspect(obj, { showHidden: true }),
        "{ [Symbol(Symbol.toStringTag)]: 'a' }"
      );

      class Foo {
        constructor() {
          this.foo = 'bar';
        }

        get [Symbol.toStringTag]() {
          return this.foo;
        }
      }

      assert.strictEqual(
        util.inspect(
          Object.create(null, { [Symbol.toStringTag]: { value: 'foo' } })
        ),
        '[Object: null prototype] [foo] {}'
      );

      assert.strictEqual(util.inspect(new Foo()), "Foo [bar] { foo: 'bar' }");

      assert.strictEqual(
        util.inspect(new (class extends Foo {})()),
        "Foo [bar] { foo: 'bar' }"
      );

      assert.strictEqual(
        util.inspect(
          Object.create(
            { __proto__: Foo.prototype },
            {
              foo: { value: 'bar', enumerable: true },
            }
          )
        ),
        "Foo [bar] { foo: 'bar' }"
      );

      class ThrowingClass {
        get [Symbol.toStringTag]() {
          throw new Error('toStringTag error');
        }
      }

      assert.throws(
        () => util.inspect(new ThrowingClass()),
        /toStringTag error/
      );

      class NotStringClass {
        get [Symbol.toStringTag]() {
          return null;
        }
      }

      assert.strictEqual(
        util.inspect(new NotStringClass()),
        'NotStringClass {}'
      );
    }

    {
      const o = {
        a: [
          1,
          2,
          [
            [
              'Lorem ipsum dolor\nsit amet,\tconsectetur adipiscing elit, sed do ' +
                'eiusmod tempor incididunt ut labore et dolore magna aliqua.',
              'test',
              'foo',
            ],
          ],
          4,
        ],
        b: new Map([
          ['za', 1],
          ['zb', 'test'],
        ]),
      };

      let out = util.inspect(o, { compact: true, depth: 5, breakLength: 80 });
      let expect = [
        '{ a:',
        '   [ 1,',
        '     2,',
        "     [ [ 'Lorem ipsum dolor\\nsit amet,\\tconsectetur adipiscing elit, " +
          "sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.',",
        "         'test',",
        "         'foo' ] ],",
        '     4 ],',
        "  b: Map(2) { 'za' => 1, 'zb' => 'test' } }",
      ].join('\n');
      assert.strictEqual(out, expect);

      out = util.inspect(o, { compact: false, depth: 5, breakLength: 60 });
      expect = [
        '{',
        '  a: [',
        '    1,',
        '    2,',
        '    [',
        '      [',
        "        'Lorem ipsum dolor\\n' +",
        "          'sit amet,\\tconsectetur adipiscing elit, sed do eiusmod " +
          "tempor incididunt ut labore et dolore magna aliqua.',",
        "        'test',",
        "        'foo'",
        '      ]',
        '    ],',
        '    4',
        '  ],',
        '  b: Map(2) {',
        "    'za' => 1,",
        "    'zb' => 'test'",
        '  }',
        '}',
      ].join('\n');
      assert.strictEqual(out, expect);

      out = util.inspect(o.a[2][0][0], { compact: false, breakLength: 30 });
      expect = [
        "'Lorem ipsum dolor\\n' +",
        "  'sit amet,\\tconsectetur adipiscing elit, sed do eiusmod tempor " +
          "incididunt ut labore et dolore magna aliqua.'",
      ].join('\n');
      assert.strictEqual(out, expect);

      out = util.inspect('12345678901234567890123456789012345678901234567890', {
        compact: false,
        breakLength: 3,
      });
      expect = "'12345678901234567890123456789012345678901234567890'";
      assert.strictEqual(out, expect);

      out = util.inspect(
        '12 45 78 01 34 67 90 23 56 89 123456789012345678901234567890',
        { compact: false, breakLength: 3 }
      );
      expect = [
        "'12 45 78 01 34 67 90 23 56 89 123456789012345678901234567890'",
      ].join('\n');
      assert.strictEqual(out, expect);

      o.a = () => {};
      o.b = new Number(3);
      out = util.inspect(o, { compact: false, breakLength: 3 });
      expect = [
        '{',
        '  a: [Function (anonymous)],',
        '  b: [Number: 3]',
        '}',
      ].join('\n');
      assert.strictEqual(out, expect);

      out = util.inspect(o, {
        compact: false,
        breakLength: 3,
        showHidden: true,
      });
      expect = [
        '{',
        '  a: [Function (anonymous)] {',
        '    [length]: 0,',
        "    [name]: ''",
        '  },',
        '  b: [Number: 3]',
        '}',
      ].join('\n');
      assert.strictEqual(out, expect);

      o[util.inspect.custom] = () => 42;
      out = util.inspect(o, { compact: false, breakLength: 3 });
      expect = '42';
      assert.strictEqual(out, expect);

      o[util.inspect.custom] = () => '12 45 78 01 34 67 90 23';
      out = util.inspect(o, { compact: false, breakLength: 3 });
      expect = '12 45 78 01 34 67 90 23';
      assert.strictEqual(out, expect);

      o[util.inspect.custom] = () => ({ a: '12 45 78 01 34 67 90 23' });
      out = util.inspect(o, { compact: false, breakLength: 3 });
      expect = "{\n  a: '12 45 78 01 34 67 90 23'\n}";
      assert.strictEqual(out, expect);
    }

    // Check compact indentation.
    {
      const typed = new Uint8Array();
      typed.buffer.foo = true;
      const set = new Set([[1, 2]]);
      const promise = Promise.resolve([[1, set]]);
      const map = new Map([[promise, typed]]);
      map.set(set.values(), map.values());

      let out = util.inspect(map, {
        compact: false,
        showHidden: true,
        depth: 9,
      });
      let expected = [
        'Map(2) {',
        '  Promise {',
        '    [',
        '      [',
        '        1,',
        '        Set(1) {',
        '          [',
        '            1,',
        '            2,',
        '            [length]: 2',
        '          ]',
        '        },',
        '        [length]: 2',
        '      ],',
        '      [length]: 1',
        '    ]',
        '  } => Uint8Array(0) [',
        '    [BYTES_PER_ELEMENT]: 1,',
        '    [length]: 0,',
        '    [byteLength]: 0,',
        '    [byteOffset]: 0,',
        '    [buffer]: ArrayBuffer {',
        '      byteLength: 0,',
        '      foo: true',
        '    }',
        '  ],',
        '  [Set Iterator] {',
        '    [',
        '      1,',
        '      2,',
        '      [length]: 2',
        '    ],',
        "    [Symbol(Symbol.toStringTag)]: 'Set Iterator'",
        '  } => <ref *1> [Map Iterator] {',
        '    Uint8Array(0) [',
        '      [BYTES_PER_ELEMENT]: 1,',
        '      [length]: 0,',
        '      [byteLength]: 0,',
        '      [byteOffset]: 0,',
        '      [buffer]: ArrayBuffer {',
        '        byteLength: 0,',
        '        foo: true',
        '      }',
        '    ],',
        '    [Circular *1],',
        "    [Symbol(Symbol.toStringTag)]: 'Map Iterator'",
        '  }',
        '}',
      ].join('\n');

      assert.strict.equal(out, expected);

      out = util.inspect(map, { compact: 2, showHidden: true, depth: 9 });

      expected = [
        'Map(2) {',
        '  Promise {',
        '    [',
        '      [',
        '        1,',
        '        Set(1) { [ 1, 2, [length]: 2 ] },',
        '        [length]: 2',
        '      ],',
        '      [length]: 1',
        '    ]',
        '  } => Uint8Array(0) [',
        '    [BYTES_PER_ELEMENT]: 1,',
        '    [length]: 0,',
        '    [byteLength]: 0,',
        '    [byteOffset]: 0,',
        '    [buffer]: ArrayBuffer { byteLength: 0, foo: true }',
        '  ],',
        '  [Set Iterator] {',
        '    [ 1, 2, [length]: 2 ],',
        "    [Symbol(Symbol.toStringTag)]: 'Set Iterator'",
        '  } => <ref *1> [Map Iterator] {',
        '    Uint8Array(0) [',
        '      [BYTES_PER_ELEMENT]: 1,',
        '      [length]: 0,',
        '      [byteLength]: 0,',
        '      [byteOffset]: 0,',
        '      [buffer]: ArrayBuffer { byteLength: 0, foo: true }',
        '    ],',
        '    [Circular *1],',
        "    [Symbol(Symbol.toStringTag)]: 'Map Iterator'",
        '  }',
        '}',
      ].join('\n');

      assert.strict.equal(out, expected);

      out = util.inspect(map, {
        showHidden: true,
        depth: 9,
        breakLength: 4,
        compact: true,
      });
      expected = [
        'Map(2) {',
        '  Promise {',
        '    [ [ 1,',
        '        Set(1) {',
        '          [ 1,',
        '            2,',
        '            [length]: 2 ] },',
        '        [length]: 2 ],',
        '      [length]: 1 ] } => Uint8Array(0) [',
        '    [BYTES_PER_ELEMENT]: 1,',
        '    [length]: 0,',
        '    [byteLength]: 0,',
        '    [byteOffset]: 0,',
        '    [buffer]: ArrayBuffer {',
        '      byteLength: 0,',
        '      foo: true } ],',
        '  [Set Iterator] {',
        '    [ 1,',
        '      2,',
        '      [length]: 2 ],',
        '    [Symbol(Symbol.toStringTag)]:',
        "     'Set Iterator' } => <ref *1> [Map Iterator] {",
        '    Uint8Array(0) [',
        '      [BYTES_PER_ELEMENT]: 1,',
        '      [length]: 0,',
        '      [byteLength]: 0,',
        '      [byteOffset]: 0,',
        '      [buffer]: ArrayBuffer {',
        '        byteLength: 0,',
        '        foo: true } ],',
        '    [Circular *1],',
        '    [Symbol(Symbol.toStringTag)]:',
        "     'Map Iterator' } }",
      ].join('\n');

      assert.strict.equal(out, expected);
    }

    {
      // Test WeakMap && WeakSet
      const obj = {};
      const arr = [];
      const weakMap = new WeakMap([
        [obj, arr],
        [arr, obj],
      ]);
      let out = util.inspect(weakMap, { showHidden: true });
      let expect = 'WeakMap { [ [length]: 0 ] => {}, {} => [ [length]: 0 ] }';
      assert.strictEqual(out, expect);

      out = util.inspect(weakMap);
      expect = 'WeakMap { <items unknown> }';
      assert.strictEqual(out, expect);

      out = util.inspect(weakMap, { maxArrayLength: 0, showHidden: true });
      expect = 'WeakMap { ... 2 more items }';
      assert.strictEqual(out, expect);

      weakMap.extra = true;
      out = util.inspect(weakMap, { maxArrayLength: 1, showHidden: true });
      // It is not possible to determine the output reliable.
      expect =
        'WeakMap { [ [length]: 0 ] => {}, ... 1 more item, extra: true }';
      let expectAlt =
        'WeakMap { {} => [ [length]: 0 ], ... 1 more item, ' + 'extra: true }';
      assert(
        out === expect || out === expectAlt,
        `Found: "${out}"\nrather than: "${expect}"\nor: "${expectAlt}"`
      );

      // Test WeakSet
      arr.push(1);
      const weakSet = new WeakSet([obj, arr]);
      out = util.inspect(weakSet, { showHidden: true });
      expect = 'WeakSet { [ 1, [length]: 1 ], {} }';
      assert.strictEqual(out, expect);

      out = util.inspect(weakSet);
      expect = 'WeakSet { <items unknown> }';
      assert.strictEqual(out, expect);

      out = util.inspect(weakSet, { maxArrayLength: -2, showHidden: true });
      expect = 'WeakSet { ... 2 more items }';
      assert.strictEqual(out, expect);

      weakSet.extra = true;
      out = util.inspect(weakSet, { maxArrayLength: 1, showHidden: true });
      // It is not possible to determine the output reliable.
      expect = 'WeakSet { {}, ... 1 more item, extra: true }';
      expectAlt =
        'WeakSet { [ 1, [length]: 1 ], ... 1 more item, extra: true }';
      assert(
        out === expect || out === expectAlt,
        `Found: "${out}"\nrather than: "${expect}"\nor: "${expectAlt}"`
      );
      // Keep references to the WeakMap entries, otherwise they could be GCed too
      // early.
      assert(obj && arr);
    }

    {
      // Test argument objects.
      const args = (function () {
        return arguments;
      })('a');
      assert.strictEqual(util.inspect(args), "[Arguments] { '0': 'a' }");
    }

    {
      // Test that a long linked list can be inspected without throwing an error.
      const list = {};
      let head = list;
      // A linked list of length 100k should be inspectable in some way, even though
      // the real cutoff value is much lower than 100k.
      for (let i = 0; i < 100000; i++) head = head.next = {};
      assert.strictEqual(
        util.inspect(list),
        '{ next: { next: { next: [Object] } } }'
      );
      const longList = util.inspect(list, { depth: Infinity });
      const match = longList.match(/next/g);
      assert(match.length > 500 && match.length < 10000);
      assert(
        longList.includes(
          '[Object: Inspection interrupted ' +
            'prematurely. Maximum call stack size exceeded.]'
        )
      );
    }

    // Do not escape single quotes if no double quote or backtick is present.
    assert.strictEqual(util.inspect("'"), '"\'"');
    assert.strictEqual(util.inspect('"\''), '`"\'`');
    // eslint-disable-next-line no-template-curly-in-string
    assert.strictEqual(util.inspect('"\'${a}'), "'\"\\'${a}'");

    // Errors should visualize as much information as possible.
    // If the name is not included in the stack, visualize it as well.
    [
      [class Foo extends TypeError {}, 'test'],
      [class Foo extends TypeError {}, undefined],
      [class BarError extends Error {}, 'test'],
      [
        class BazError extends Error {
          get name() {
            return 'BazError';
          }
        },
        undefined,
      ],
    ].forEach(([Class, message], i) => {
      console.log('Test %i', i);
      const foo = new Class(message);
      const name = foo.name;
      const extra = Class.name.includes('Error') ? '' : ` [${foo.name}]`;
      assert(
        util
          .inspect(foo)
          .startsWith(
            `${Class.name}${extra}${message ? `: ${message}` : '\n'}`
          ),
        util.inspect(foo)
      );
      Object.defineProperty(foo, Symbol.toStringTag, {
        value: 'WOW',
        writable: true,
        configurable: true,
      });
      const stack = foo.stack;
      foo.stack = 'This is a stack';
      assert.strictEqual(util.inspect(foo), '[This is a stack]');
      foo.stack = stack;
      assert(
        util
          .inspect(foo)
          .startsWith(
            `${Class.name} [WOW]${extra}${message ? `: ${message}` : '\n'}`
          ),
        util.inspect(foo)
      );
      Object.setPrototypeOf(foo, null);
      assert(
        util
          .inspect(foo)
          .startsWith(
            `[${name}: null prototype] [WOW]${message ? `: ${message}` : '\n'}`
          ),
        util.inspect(foo)
      );
      foo.bar = true;
      delete foo[Symbol.toStringTag];
      assert(
        util
          .inspect(foo)
          .startsWith(
            `[${name}: null prototype]${message ? `: ${message}` : '\n'}`
          ),
        util.inspect(foo)
      );
      foo.stack = 'This is a stack';
      assert.strictEqual(
        util.inspect(foo),
        '[[Error: null prototype]: This is a stack] { bar: true }'
      );
      foo.stack = stack.split('\n')[0];
      assert.strictEqual(
        util.inspect(foo),
        `[[${name}: null prototype]${message ? `: ${message}` : ''}] { bar: true }`
      );
    });

    // Verify that classes are properly inspected.
    [
      /* eslint-disable spaced-comment, no-multi-spaces, brace-style */
      // The whitespace is intentional.
      [class {}, '[class (anonymous)]'],
      [
        class extends Error {
          log() {}
        },
        '[class (anonymous) extends Error]',
      ],
      [
        class A {
          constructor(a) {
            this.a = a;
          }
          log() {
            return this.a;
          }
        },
        '[class A]',
      ],
      [
        class // Random { // comments /* */ are part of the toString() result
          /* eslint-disable-next-line space-before-blocks */
          äß /**/
          extends /*{*/ TypeError {},
        '[class äß extends TypeError]',
      ],
      /* The whitespace and new line is intended! */
      // Foobar !!!
      [
        class X extends /****/ Error {
          // More comments
        },
        '[class X extends Error]',
      ],
      /* eslint-enable spaced-comment, no-multi-spaces, brace-style */
    ].forEach(([clazz, string]) => {
      const inspected = util.inspect(clazz);
      assert.strictEqual(inspected, string);
      Object.defineProperty(clazz, Symbol.toStringTag, {
        value: 'Woohoo',
      });
      const parts = inspected.slice(0, -1).split(' ');
      const [, name, ...rest] = parts;
      rest.unshift('[Woohoo]');
      if (rest.length) {
        rest[rest.length - 1] += ']';
      }
      assert.strictEqual(
        util.inspect(clazz),
        ['[class', name, ...rest].join(' ')
      );
      if (rest.length) {
        rest[rest.length - 1] = rest[rest.length - 1].slice(0, -1);
        rest.length = 1;
      }
      Object.setPrototypeOf(clazz, Map.prototype);
      assert.strictEqual(
        util.inspect(clazz),
        ['[class', name, '[Map]', ...rest].join(' ') + ']'
      );
      Object.setPrototypeOf(clazz, null);
      assert.strictEqual(
        util.inspect(clazz),
        ['[class', name, ...rest, 'extends [null prototype]]'].join(' ')
      );
      Object.defineProperty(clazz, 'name', { value: 'Foo' });
      const res = ['[class', 'Foo', ...rest, 'extends [null prototype]]'].join(
        ' '
      );
      assert.strictEqual(util.inspect(clazz), res);
      clazz.foo = true;
      assert.strictEqual(util.inspect(clazz), `${res} { foo: true }`);
    });

    // "class" properties should not be detected as "class".
    {
      // eslint-disable-next-line space-before-function-paren
      let obj = { class() {} };
      assert.strictEqual(util.inspect(obj), '{ class: [Function: class] }');
      obj = { class: () => {} };
      assert.strictEqual(util.inspect(obj), '{ class: [Function: class] }');
      obj = { ['class Foo {}']() {} };
      assert.strictEqual(
        util.inspect(obj),
        "{ 'class Foo {}': [Function: class Foo {}] }"
      );
      function Foo() {}
      Object.defineProperty(Foo, 'toString', { value: () => 'class Foo {}' });
      assert.strictEqual(util.inspect(Foo), '[Function: Foo]');
      function fn() {}
      Object.defineProperty(fn, 'name', { value: 'class Foo {}' });
      assert.strictEqual(util.inspect(fn), '[Function: class Foo {}]');
    }

    // Verify that throwing in valueOf and toString still produces nice results.
    [
      [new String(55), "[String: '55']"],
      [new Boolean(true), '[Boolean: true]'],
      [new Number(55), '[Number: 55]'],
      [Object(BigInt(55)), '[BigInt: 55n]'],
      [Object(Symbol('foo')), '[Symbol: Symbol(foo)]'],
      [function () {}, '[Function (anonymous)]'],
      [() => {}, '[Function (anonymous)]'],
      [[1, 2], '[ 1, 2 ]'],
      // eslint-disable-next-line no-sparse-arrays
      [[, , 5, , , ,], '[ <2 empty items>, 5, <3 empty items> ]'],
      [{ a: 5 }, '{ a: 5 }'],
      [new Set([1, 2]), 'Set(2) { 1, 2 }'],
      [new Map([[1, 2]]), 'Map(1) { 1 => 2 }'],
      [new Set([1, 2]).entries(), '[Set Entries] { [ 1, 1 ], [ 2, 2 ] }'],
      [new Map([[1, 2]]).keys(), '[Map Iterator] { 1 }'],
      [new Date(2000), '1970-01-01T00:00:02.000Z'],
      [new Uint8Array(2), 'Uint8Array(2) [ 0, 0 ]'],
      [
        new Promise((resolve) => setTimeout(resolve, 10)),
        'Promise { <pending> }',
      ],
      [new WeakSet(), 'WeakSet { <items unknown> }'],
      [new WeakMap(), 'WeakMap { <items unknown> }'],
      [/foobar/g, '/foobar/g'],
    ].forEach(([value, expected]) => {
      Object.defineProperty(value, 'valueOf', {
        get() {
          throw new Error('valueOf');
        },
      });
      Object.defineProperty(value, 'toString', {
        get() {
          throw new Error('toString');
        },
      });
      assert.strictEqual(util.inspect(value), expected);
      value.foo = 'bar';
      assert.notStrictEqual(util.inspect(value), expected);
      delete value.foo;
      value[Symbol('foo')] = 'yeah';
      assert.notStrictEqual(util.inspect(value), expected);
    });

    // Verify that having no prototype still produces nice results.
    [
      [[1, 3, 4], '[Array(3): null prototype] [ 1, 3, 4 ]'],
      [new Set([1, 2]), '[Set(2): null prototype] { 1, 2 }'],
      [new Map([[1, 2]]), '[Map(1): null prototype] { 1 => 2 }'],
      [
        new Promise((resolve) => setTimeout(resolve, 10)),
        '[Promise: null prototype] { <pending> }',
      ],
      [new WeakSet(), '[WeakSet: null prototype] { <items unknown> }'],
      [new WeakMap(), '[WeakMap: null prototype] { <items unknown> }'],
      [new Uint8Array(2), '[Uint8Array(2): null prototype] [ 0, 0 ]'],
      [new Uint16Array(2), '[Uint16Array(2): null prototype] [ 0, 0 ]'],
      [new Uint32Array(2), '[Uint32Array(2): null prototype] [ 0, 0 ]'],
      [new Int8Array(2), '[Int8Array(2): null prototype] [ 0, 0 ]'],
      [new Int16Array(2), '[Int16Array(2): null prototype] [ 0, 0 ]'],
      [new Int32Array(2), '[Int32Array(2): null prototype] [ 0, 0 ]'],
      [new Float32Array(2), '[Float32Array(2): null prototype] [ 0, 0 ]'],
      [new Float64Array(2), '[Float64Array(2): null prototype] [ 0, 0 ]'],
      [new BigInt64Array(2), '[BigInt64Array(2): null prototype] [ 0n, 0n ]'],
      [new BigUint64Array(2), '[BigUint64Array(2): null prototype] [ 0n, 0n ]'],
      [
        new ArrayBuffer(16),
        '[ArrayBuffer: null prototype] {\n' +
          '  [Uint8Contents]: <00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00>,\n' +
          '  byteLength: undefined\n}',
      ],
      [
        new DataView(new ArrayBuffer(16)),
        '[DataView: null prototype] {\n  byteLength: undefined,\n  ' +
          'byteOffset: undefined,\n  buffer: undefined\n}',
      ],
      [
        new SharedArrayBuffer(2),
        '[SharedArrayBuffer: null prototype] ' +
          '{\n  [Uint8Contents]: <00 00>,\n  byteLength: undefined\n}',
      ],
      [/foobar/, '[RegExp: null prototype] /foobar/'],
      [
        new Date('Sun, 14 Feb 2010 11:48:40 GMT'),
        '[Date: null prototype] 2010-02-14T11:48:40.000Z',
      ],
    ].forEach(([value, expected]) => {
      assert.strictEqual(
        util.inspect(Object.setPrototypeOf(value, null)),
        expected
      );
      value.foo = 'bar';
      assert.notStrictEqual(util.inspect(value), expected);
      delete value.foo;
      value[Symbol('foo')] = 'yeah';
      assert.notStrictEqual(util.inspect(value), expected);
    });

    // Verify that subclasses with and without prototype produce nice results.
    [
      [RegExp, ['foobar', 'g'], '/foobar/g'],
      [WeakSet, [[{}]], '{ <items unknown> }'],
      [WeakMap, [[[{}, {}]]], '{ <items unknown> }'],
      [
        BigInt64Array,
        [10],
        '[\n  0n, 0n, 0n, 0n, 0n,\n  0n, 0n, 0n, 0n, 0n\n]',
      ],
      [Date, ['Sun, 14 Feb 2010 11:48:40 GMT'], '2010-02-14T11:48:40.000Z'],
      [Date, ['invalid_date'], 'Invalid Date'],
    ].forEach(([base, input, rawExpected]) => {
      class Foo extends base {}
      const value = new Foo(...input);
      const symbol = value[Symbol.toStringTag];
      const size = base.name.includes('Array') ? `(${input[0]})` : '';
      const expected = `Foo${size} ${symbol ? `[${symbol}] ` : ''}${rawExpected}`;
      const expectedWithoutProto = `[${base.name}${size}: null prototype] ${rawExpected}`;
      assert.strictEqual(util.inspect(value), expected);
      value.foo = 'bar';
      assert.notStrictEqual(util.inspect(value), expected);
      delete value.foo;
      assert.strictEqual(
        util.inspect(Object.setPrototypeOf(value, null)),
        expectedWithoutProto
      );
      value.foo = 'bar';
      let res = util.inspect(value);
      assert.notStrictEqual(res, expectedWithoutProto);
      assert.match(res, /foo: 'bar'/);
      delete value.foo;
      value[Symbol('foo')] = 'yeah';
      res = util.inspect(value);
      assert.notStrictEqual(res, expectedWithoutProto);
      assert.match(res, /\[Symbol\(foo\)]: 'yeah'/);
    });

    assert.strictEqual(inspect(1n), '1n');
    assert.strictEqual(inspect(Object(-1n)), '[BigInt: -1n]');
    assert.strictEqual(inspect(Object(13n)), '[BigInt: 13n]');
    assert.strictEqual(
      inspect(new BigInt64Array([0n])),
      'BigInt64Array(1) [ 0n ]'
    );
    assert.strictEqual(
      inspect(new BigUint64Array([0n])),
      'BigUint64Array(1) [ 0n ]'
    );

    // Verify non-enumerable keys get escaped.
    {
      const obj = {};
      Object.defineProperty(obj, 'Non\nenumerable\tkey', { value: true });
      assert.strictEqual(
        util.inspect(obj, { showHidden: true }),
        '{ [Non\\nenumerable\\tkey]: true }'
      );
    }

    // Check for special colors.
    {
      const special = inspect.colors[inspect.styles.special];
      const string = inspect.colors[inspect.styles.string];

      assert.strictEqual(
        inspect(new WeakSet(), { colors: true }),
        `WeakSet { \u001b[${special[0]}m<items unknown>\u001b[${special[1]}m }`
      );
      assert.strictEqual(
        inspect(new WeakMap(), { colors: true }),
        `WeakMap { \u001b[${special[0]}m<items unknown>\u001b[${special[1]}m }`
      );
      assert.strictEqual(
        inspect(new Promise(() => {}), { colors: true }),
        `Promise { \u001b[${special[0]}m<pending>\u001b[${special[1]}m }`
      );

      const rejection = Promise.reject('Oh no!');
      assert.strictEqual(
        inspect(rejection, { colors: true }),
        `Promise { \u001b[${special[0]}m<rejected>\u001b[${special[1]}m ` +
          `\u001b[${string[0]}m'Oh no!'\u001b[${string[1]}m }`
      );
      rejection.catch(() => {});

      // Verify that aliases do not show up as key while checking `inspect.colors`.
      const colors = Object.keys(inspect.colors);
      const aliases = Object.getOwnPropertyNames(inspect.colors).filter(
        (c) => !colors.includes(c)
      );
      assert(!colors.includes('grey'));
      assert(colors.includes('gray'));
      // Verify that all aliases are correctly mapped.
      for (const alias of aliases) {
        assert(Array.isArray(inspect.colors[alias]));
      }
      // Check consistent naming.
      [
        'black',
        'red',
        'green',
        'yellow',
        'blue',
        'magenta',
        'cyan',
        'white',
      ].forEach((color, i) => {
        assert.deepStrictEqual(inspect.colors[color], [30 + i, 39]);
        assert.deepStrictEqual(inspect.colors[`${color}Bright`], [90 + i, 39]);
        const bgColor = `bg${color[0].toUpperCase()}${color.slice(1)}`;
        assert.deepStrictEqual(inspect.colors[bgColor], [40 + i, 49]);
        assert.deepStrictEqual(inspect.colors[`${bgColor}Bright`], [
          100 + i,
          49,
        ]);
      });

      // Unknown colors are handled gracefully:
      const stringStyle = inspect.styles.string;
      inspect.styles.string = 'UNKNOWN';
      assert.strictEqual(inspect('foobar', { colors: true }), "'foobar'");
      inspect.styles.string = stringStyle;
    }

    assert.strictEqual(
      inspect([1, 3, 2], { sorted: true }),
      inspect([1, 3, 2])
    );
    assert.strictEqual(
      inspect({ c: 3, a: 1, b: 2 }, { sorted: true }),
      '{ a: 1, b: 2, c: 3 }'
    );
    assert.strictEqual(
      inspect(
        { a200: 4, a100: 1, a102: 3, a101: 2 },
        {
          sorted(a, b) {
            return b.localeCompare(a);
          },
        }
      ),
      '{ a200: 4, a102: 3, a101: 2, a100: 1 }'
    );

    // Non-indices array properties are sorted as well.
    {
      const arr = [3, 2, 1];
      arr.b = 2;
      arr.c = 3;
      arr.a = 1;
      arr[Symbol('b')] = true;
      arr[Symbol('a')] = false;
      assert.strictEqual(
        inspect(arr, { sorted: true }),
        '[ 3, 2, 1, [Symbol(a)]: false, [Symbol(b)]: true, a: 1, b: 2, c: 3 ]'
      );
    }

    // Manipulate the prototype in weird ways.
    {
      let obj = { a: true };
      let value = (function () {
        return function () {};
      })();
      Object.setPrototypeOf(value, null);
      Object.setPrototypeOf(obj, value);
      assert.strictEqual(
        util.inspect(obj),
        'Object <[Function (null prototype) (anonymous)]> { a: true }'
      );
      assert.strictEqual(
        util.inspect(obj, { colors: true }),
        'Object <\u001b[36m[Function (null prototype) (anonymous)]\u001b[39m> ' +
          '{ a: \u001b[33mtrue\u001b[39m }'
      );

      obj = { a: true };
      value = [];
      Object.setPrototypeOf(value, null);
      Object.setPrototypeOf(obj, value);
      assert.strictEqual(
        util.inspect(obj),
        'Object <[Array(0): null prototype] []> { a: true }'
      );

      function StorageObject() {}
      StorageObject.prototype = { __proto__: null };
      assert.strictEqual(
        util.inspect(new StorageObject()),
        'StorageObject <[Object: null prototype] {}> {}'
      );

      obj = [1, 2, 3];
      Object.setPrototypeOf(obj, Number.prototype);
      assert.strictEqual(inspect(obj), "Number { '0': 1, '1': 2, '2': 3 }");

      Object.setPrototypeOf(obj, { __proto__: null });
      assert.strictEqual(
        inspect(obj),
        "Array <[Object: null prototype] {}> { '0': 1, '1': 2, '2': 3 }"
      );

      StorageObject.prototype = { __proto__: null };
      Object.setPrototypeOf(StorageObject.prototype, { __proto__: null });
      Object.setPrototypeOf(Object.getPrototypeOf(StorageObject.prototype), {
        __proto__: null,
      });
      assert.strictEqual(
        util.inspect(new StorageObject()),
        'StorageObject <Object <Object <[Object: null prototype] {}>>> {}'
      );
      assert.strictEqual(
        util.inspect(new StorageObject(), { depth: 1 }),
        'StorageObject <Object <Object <Complex prototype>>> {}'
      );
    }

    // Check that the fallback always works.
    {
      const obj = new Set([1, 2]);
      const iterator = obj[Symbol.iterator];
      Object.setPrototypeOf(obj, null);
      Object.defineProperty(obj, Symbol.iterator, {
        value: iterator,
        configurable: true,
      });
      assert.strictEqual(
        util.inspect(obj),
        '[Set(2): null prototype] { 1, 2 }'
      );
      Object.defineProperty(obj, Symbol.iterator, {
        value: true,
        configurable: true,
      });
      Object.defineProperty(obj, 'size', {
        value: NaN,
        configurable: true,
        enumerable: true,
      });
      assert.strictEqual(
        util.inspect(obj),
        '[Set(2): null prototype] { 1, 2, size: NaN }'
      );
    }

    // Check the getter option.
    {
      let foo = 1;
      const get = {
        get foo() {
          return foo;
        },
      };
      const getset = {
        get foo() {
          return foo;
        },
        set foo(val) {
          foo = val;
        },
        get inc() {
          return ++foo;
        },
      };
      const thrower = {
        get foo() {
          throw new Error('Oops');
        },
      };
      assert.strictEqual(
        inspect(get, { getters: true, colors: true }),
        '{ foo: \u001b[36m[Getter:\u001b[39m ' +
          '\u001b[33m1\u001b[39m\u001b[36m]\u001b[39m }'
      );
      assert.strictEqual(
        inspect(thrower, { getters: true }),
        '{ foo: [Getter: <Inspection threw (Oops)>] }'
      );
      assert.strictEqual(
        inspect(getset, { getters: true }),
        '{ foo: [Getter/Setter: 1], inc: [Getter: 2] }'
      );
      assert.strictEqual(
        inspect(getset, { getters: 'get' }),
        '{ foo: [Getter/Setter], inc: [Getter: 3] }'
      );
      assert.strictEqual(
        inspect(getset, { getters: 'set' }),
        '{ foo: [Getter/Setter: 3], inc: [Getter] }'
      );
      getset.foo = new Set([[{ a: true }, 2, {}], 'foobar', { x: 1 }]);
      assert.strictEqual(
        inspect(getset, { getters: true }),
        '{\n  foo: [Getter/Setter] Set(3) { [ [Object], 2, {} ], ' +
          "'foobar', { x: 1 } },\n  inc: [Getter: NaN]\n}"
      );
    }

    // Check compact number mode.
    {
      let obj = {
        a: {
          b: {
            x: 5,
            c: {
              x: '10000000000000000 00000000000000000 '.repeat(1e1),
              d: 2,
              e: 3,
            },
          },
        },
        b: [1, 2, [1, 2, { a: 1, b: 2, c: 3 }]],
        c: ['foo', 4, 444444],
        d: Array.from({ length: 101 }).map((e, i) => {
          return i % 2 === 0 ? i * i : i;
        }),
        e: Array(6).fill('foobar'),
        f: Array(9).fill('foobar'),
        g: Array(21).fill('foobar baz'),
        h: [100].concat(Array.from({ length: 9 }).map((e, n) => n)),
        long: Array(9).fill('This text is too long for grouping!'),
      };

      let out = util.inspect(obj, { compact: 3, depth: 10, breakLength: 60 });
      let expected = [
        '{',
        '  a: {',
        '    b: {',
        '      x: 5,',
        '      c: {',
        "        x: '10000000000000000 00000000000000000 10000000000000000 " +
          '00000000000000000 10000000000000000 00000000000000000 ' +
          '10000000000000000 00000000000000000 10000000000000000 ' +
          '00000000000000000 10000000000000000 00000000000000000 ' +
          '10000000000000000 00000000000000000 10000000000000000 ' +
          '00000000000000000 10000000000000000 00000000000000000 ' +
          "10000000000000000 00000000000000000 ',",
        '        d: 2,',
        '        e: 3',
        '      }',
        '    }',
        '  },',
        '  b: [ 1, 2, [ 1, 2, { a: 1, b: 2, c: 3 } ] ],',
        "  c: [ 'foo', 4, 444444 ],",
        '  d: [',
        '       0,    1,    4,    3,   16,    5,   36,    7,   64,',
        '       9,  100,   11,  144,   13,  196,   15,  256,   17,',
        '     324,   19,  400,   21,  484,   23,  576,   25,  676,',
        '      27,  784,   29,  900,   31, 1024,   33, 1156,   35,',
        '    1296,   37, 1444,   39, 1600,   41, 1764,   43, 1936,',
        '      45, 2116,   47, 2304,   49, 2500,   51, 2704,   53,',
        '    2916,   55, 3136,   57, 3364,   59, 3600,   61, 3844,',
        '      63, 4096,   65, 4356,   67, 4624,   69, 4900,   71,',
        '    5184,   73, 5476,   75, 5776,   77, 6084,   79, 6400,',
        '      81, 6724,   83, 7056,   85, 7396,   87, 7744,   89,',
        '    8100,   91, 8464,   93, 8836,   95, 9216,   97, 9604,',
        '      99,',
        '    ... 1 more item',
        '  ],',
        '  e: [',
        "    'foobar',",
        "    'foobar',",
        "    'foobar',",
        "    'foobar',",
        "    'foobar',",
        "    'foobar'",
        '  ],',
        '  f: [',
        "    'foobar', 'foobar',",
        "    'foobar', 'foobar',",
        "    'foobar', 'foobar',",
        "    'foobar', 'foobar',",
        "    'foobar'",
        '  ],',
        '  g: [',
        "    'foobar baz', 'foobar baz',",
        "    'foobar baz', 'foobar baz',",
        "    'foobar baz', 'foobar baz',",
        "    'foobar baz', 'foobar baz',",
        "    'foobar baz', 'foobar baz',",
        "    'foobar baz', 'foobar baz',",
        "    'foobar baz', 'foobar baz',",
        "    'foobar baz', 'foobar baz',",
        "    'foobar baz', 'foobar baz',",
        "    'foobar baz', 'foobar baz',",
        "    'foobar baz'",
        '  ],',
        '  h: [',
        '    100, 0, 1, 2, 3,',
        '      4, 5, 6, 7, 8',
        '  ],',
        '  long: [',
        "    'This text is too long for grouping!',",
        "    'This text is too long for grouping!',",
        "    'This text is too long for grouping!',",
        "    'This text is too long for grouping!',",
        "    'This text is too long for grouping!',",
        "    'This text is too long for grouping!',",
        "    'This text is too long for grouping!',",
        "    'This text is too long for grouping!',",
        "    'This text is too long for grouping!'",
        '  ]',
        '}',
      ].join('\n');

      assert.strictEqual(out, expected);

      obj = [
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 123456789,
      ];

      out = util.inspect(obj, { compact: 3 });

      expected = [
        '[',
        '  1, 1,         1, 1,',
        '  1, 1,         1, 1,',
        '  1, 1,         1, 1,',
        '  1, 1,         1, 1,',
        '  1, 1,         1, 1,',
        '  1, 1,         1, 1,',
        '  1, 1, 123456789',
        ']',
      ].join('\n');

      assert.strictEqual(out, expected);

      // Unicode support. あ has a length of one and a width of two.
      obj = [
        '123',
        '123',
        '123',
        '123',
        'あああ',
        '123',
        '123',
        '123',
        '123',
        'あああ',
      ];

      out = util.inspect(obj, { compact: 3 });

      expected = [
        '[',
        "  '123',    '123',",
        "  '123',    '123',",
        "  'あああ', '123',",
        "  '123',    '123',",
        "  '123',    'あああ'",
        ']',
      ].join('\n');

      assert.strictEqual(out, expected);

      // Array grouping should prevent lining up outer elements on a single line.
      obj = [[[1, 2, 3, 4, 5, 6, 7, 8, 9]]];

      out = util.inspect(obj, { compact: 3 });

      expected = [
        '[',
        '  [',
        '    [',
        '      1, 2, 3, 4, 5,',
        '      6, 7, 8, 9',
        '    ]',
        '  ]',
        ']',
      ].join('\n');

      assert.strictEqual(out, expected);

      // Verify that array grouping and line consolidation does not happen together.
      obj = {
        a: {
          b: {
            x: 5,
            c: {
              d: 2,
              e: 3,
            },
          },
        },
        b: Array.from({ length: 9 }).map((e, n) => {
          return n % 2 === 0 ? 'foobar' : 'baz';
        }),
      };

      out = util.inspect(obj, {
        compact: 1,
        breakLength: Infinity,
        colors: true,
      });

      expected = [
        '{',
        '  a: {',
        '    b: { x: \u001b[33m5\u001b[39m, c: \u001b[36m[Object]\u001b[39m }',
        '  },',
        '  b: [',
        "    \u001b[32m'foobar'\u001b[39m, \u001b[32m'baz'\u001b[39m,",
        "    \u001b[32m'foobar'\u001b[39m, \u001b[32m'baz'\u001b[39m,",
        "    \u001b[32m'foobar'\u001b[39m, \u001b[32m'baz'\u001b[39m,",
        "    \u001b[32m'foobar'\u001b[39m, \u001b[32m'baz'\u001b[39m,",
        "    \u001b[32m'foobar'\u001b[39m",
        '  ]',
        '}',
      ].join('\n');

      assert.strictEqual(out, expected);

      obj = Array.from({ length: 60 }).map((e, i) => i);
      out = util.inspect(obj, {
        compact: 1,
        breakLength: Infinity,
        colors: true,
      });

      expected = [
        '[',
        '   \u001b[33m0\u001b[39m,  \u001b[33m1\u001b[39m,  \u001b[33m2\u001b[39m,  \u001b[33m3\u001b[39m,',
        '   \u001b[33m4\u001b[39m,  \u001b[33m5\u001b[39m,  \u001b[33m6\u001b[39m,  \u001b[33m7\u001b[39m,',
        '   \u001b[33m8\u001b[39m,  \u001b[33m9\u001b[39m, \u001b[33m10\u001b[39m, \u001b[33m11\u001b[39m,',
        '  \u001b[33m12\u001b[39m, \u001b[33m13\u001b[39m, \u001b[33m14\u001b[39m, \u001b[33m15\u001b[39m,',
        '  \u001b[33m16\u001b[39m, \u001b[33m17\u001b[39m, \u001b[33m18\u001b[39m, \u001b[33m19\u001b[39m,',
        '  \u001b[33m20\u001b[39m, \u001b[33m21\u001b[39m, \u001b[33m22\u001b[39m, \u001b[33m23\u001b[39m,',
        '  \u001b[33m24\u001b[39m, \u001b[33m25\u001b[39m, \u001b[33m26\u001b[39m, \u001b[33m27\u001b[39m,',
        '  \u001b[33m28\u001b[39m, \u001b[33m29\u001b[39m, \u001b[33m30\u001b[39m, \u001b[33m31\u001b[39m,',
        '  \u001b[33m32\u001b[39m, \u001b[33m33\u001b[39m, \u001b[33m34\u001b[39m, \u001b[33m35\u001b[39m,',
        '  \u001b[33m36\u001b[39m, \u001b[33m37\u001b[39m, \u001b[33m38\u001b[39m, \u001b[33m39\u001b[39m,',
        '  \u001b[33m40\u001b[39m, \u001b[33m41\u001b[39m, \u001b[33m42\u001b[39m, \u001b[33m43\u001b[39m,',
        '  \u001b[33m44\u001b[39m, \u001b[33m45\u001b[39m, \u001b[33m46\u001b[39m, \u001b[33m47\u001b[39m,',
        '  \u001b[33m48\u001b[39m, \u001b[33m49\u001b[39m, \u001b[33m50\u001b[39m, \u001b[33m51\u001b[39m,',
        '  \u001b[33m52\u001b[39m, \u001b[33m53\u001b[39m, \u001b[33m54\u001b[39m, \u001b[33m55\u001b[39m,',
        '  \u001b[33m56\u001b[39m, \u001b[33m57\u001b[39m, \u001b[33m58\u001b[39m, \u001b[33m59\u001b[39m',
        ']',
      ].join('\n');

      assert.strictEqual(out, expected);

      out = util.inspect([1, 2, 3, 4], { compact: 1, colors: true });
      expected =
        '[ \u001b[33m1\u001b[39m, \u001b[33m2\u001b[39m, ' +
        '\u001b[33m3\u001b[39m, \u001b[33m4\u001b[39m ]';

      assert.strictEqual(out, expected);

      obj = [
        'Object',
        'Function',
        'Array',
        'Number',
        'parseFloat',
        'parseInt',
        'Infinity',
        'NaN',
        'undefined',
        'Boolean',
        'String',
        'Symbol',
        'Date',
        'Promise',
        'RegExp',
        'Error',
        'EvalError',
        'RangeError',
        'ReferenceError',
        'SyntaxError',
        'TypeError',
        'URIError',
        'JSON',
        'Math',
        'console',
        'Intl',
        'ArrayBuffer',
        'Uint8Array',
        'Int8Array',
        'Uint16Array',
        'Int16Array',
        'Uint32Array',
        'Int32Array',
        'Float32Array',
        'Float64Array',
        'Uint8ClampedArray',
        'BigUint64Array',
        'BigInt64Array',
        'DataView',
        'Map',
        'BigInt',
        'Set',
        'WeakMap',
        'WeakSet',
        'Proxy',
        'Reflect',
        'decodeURI',
        'decodeURIComponent',
        'encodeURI',
        'encodeURIComponent',
        'escape',
        'unescape',
        'eval',
        'isFinite',
        'isNaN',
        'SharedArrayBuffer',
        'Atomics',
        'globalThis',
        'WebAssembly',
        'global',
        'process',
        'Buffer',
        'URL',
        'URLSearchParams',
        'TextEncoder',
        'TextDecoder',
        'clearInterval',
        'clearTimeout',
        'setInterval',
        'setTimeout',
        'queueMicrotask',
        'clearImmediate',
        'setImmediate',
        'module',
        'require',
        'assert',
        'async_hooks',
        'buffer',
        'child_process',
        'cluster',
        'crypto',
        'dgram',
        'dns',
        'domain',
        'events',
        'fs',
        'http',
        'http2',
        'https',
        'inspector',
        'net',
        'os',
        'path',
        'perf_hooks',
        'punycode',
        'querystring',
        'readline',
        'repl',
        'stream',
        'string_decoder',
        'tls',
        'trace_events',
        'tty',
        'url',
        'v8',
        'vm',
        'worker_threads',
        'zlib',
        '_',
        '_error',
        'util',
      ];

      out = util.inspect(obj, {
        compact: 3,
        breakLength: 80,
        maxArrayLength: 250,
      });
      expected = [
        '[',
        "  'Object',          'Function',           'Array',",
        "  'Number',          'parseFloat',         'parseInt',",
        "  'Infinity',        'NaN',                'undefined',",
        "  'Boolean',         'String',             'Symbol',",
        "  'Date',            'Promise',            'RegExp',",
        "  'Error',           'EvalError',          'RangeError',",
        "  'ReferenceError',  'SyntaxError',        'TypeError',",
        "  'URIError',        'JSON',               'Math',",
        "  'console',         'Intl',               'ArrayBuffer',",
        "  'Uint8Array',      'Int8Array',          'Uint16Array',",
        "  'Int16Array',      'Uint32Array',        'Int32Array',",
        "  'Float32Array',    'Float64Array',       'Uint8ClampedArray',",
        "  'BigUint64Array',  'BigInt64Array',      'DataView',",
        "  'Map',             'BigInt',             'Set',",
        "  'WeakMap',         'WeakSet',            'Proxy',",
        "  'Reflect',         'decodeURI',          'decodeURIComponent',",
        "  'encodeURI',       'encodeURIComponent', 'escape',",
        "  'unescape',        'eval',               'isFinite',",
        "  'isNaN',           'SharedArrayBuffer',  'Atomics',",
        "  'globalThis',      'WebAssembly',        'global',",
        "  'process',         'Buffer',             'URL',",
        "  'URLSearchParams', 'TextEncoder',        'TextDecoder',",
        "  'clearInterval',   'clearTimeout',       'setInterval',",
        "  'setTimeout',      'queueMicrotask',     'clearImmediate',",
        "  'setImmediate',    'module',             'require',",
        "  'assert',          'async_hooks',        'buffer',",
        "  'child_process',   'cluster',            'crypto',",
        "  'dgram',           'dns',                'domain',",
        "  'events',          'fs',                 'http',",
        "  'http2',           'https',              'inspector',",
        "  'net',             'os',                 'path',",
        "  'perf_hooks',      'punycode',           'querystring',",
        "  'readline',        'repl',               'stream',",
        "  'string_decoder',  'tls',                'trace_events',",
        "  'tty',             'url',                'v8',",
        "  'vm',              'worker_threads',     'zlib',",
        "  '_',               '_error',             'util'",
        ']',
      ].join('\n');

      assert.strictEqual(out, expected);
    }

    {
      // Cross platform checks.
      const err = new Error('foo');
      util
        .inspect(err, { colors: true })
        .split('\n')
        .forEach((line, i) => {
          assert(i < 2 || line.startsWith('\u001b[90m'));
        });
    }

    // Inspect prototype properties.
    {
      class Foo extends Map {
        prop = false;
        prop2 = true;
        get abc() {
          return true;
        }
        get def() {
          return false;
        }
        set def(v) {}
        get xyz() {
          return 'Should be ignored';
        }
        func(a) {}
        [util.inspect.custom]() {
          return this;
        }
      }

      class Bar extends Foo {
        abc = true;
        prop = true;
        get xyz() {
          return 'YES!';
        }
        [util.inspect.custom]() {
          return this;
        }
      }

      const bar = new Bar();

      assert.strictEqual(
        inspect(bar),
        'Bar(0) [Map] { prop: true, prop2: true, abc: true }'
      );
      assert.strictEqual(
        inspect(bar, { showHidden: true, getters: true, colors: false }),
        'Bar(0) [Map] {\n' +
          '  prop: true,\n' +
          '  prop2: true,\n' +
          '  abc: true,\n' +
          "  [xyz]: [Getter: 'YES!'],\n" +
          '  [def]: [Getter/Setter: false]\n' +
          '}'
      );
      assert.strictEqual(
        inspect(bar, { showHidden: true, getters: false, colors: true }),
        'Bar(0) [Map] {\n' +
          '  prop: \x1B[33mtrue\x1B[39m,\n' +
          '  prop2: \x1B[33mtrue\x1B[39m,\n' +
          '  abc: \x1B[33mtrue\x1B[39m,\n' +
          '  \x1B[2m[xyz]: \x1B[36m[Getter]\x1B[39m\x1B[22m,\n' +
          '  \x1B[2m[def]: \x1B[36m[Getter/Setter]\x1B[39m\x1B[22m\n' +
          '}'
      );

      const obj = { __proto__: { abc: true, def: 5, toString() {} } };
      assert.strictEqual(
        inspect(obj, { showHidden: true, colors: true }),
        '{ \x1B[2mabc: \x1B[33mtrue\x1B[39m\x1B[22m, ' +
          '\x1B[2mdef: \x1B[33m5\x1B[39m\x1B[22m }'
      );

      assert.strictEqual(
        inspect(Object.getPrototypeOf(bar), {
          showHidden: true,
          getters: true,
        }),
        '<ref *1> Foo [Map] {\n' +
          '    [constructor]: [class Bar extends Foo] {\n' +
          '      [length]: 0,\n' +
          "      [name]: 'Bar',\n" +
          '      [prototype]: [Circular *1],\n' +
          '      [Symbol(Symbol.species)]: [Getter: <Inspection threw ' +
          "(Symbol.prototype.toString requires that 'this' be a Symbol)>]\n" +
          '    },\n' +
          "    [xyz]: [Getter: 'YES!'],\n" +
          '    [Symbol(nodejs.util.inspect.custom)]: ' +
          '[Function: [nodejs.util.inspect.custom]] {\n' +
          '      [length]: 0,\n' +
          "      [name]: '[nodejs.util.inspect.custom]'\n" +
          '    },\n' +
          '    [abc]: [Getter: true],\n' +
          '    [def]: [Getter/Setter: false]\n' +
          '  }'
      );

      assert.strictEqual(inspect(Object.getPrototypeOf(bar)), 'Foo [Map] {}');

      assert.strictEqual(inspect(Object.getPrototypeOf(new Foo())), 'Map {}');
    }

    // Check that prototypes with a null prototype are inspectable.
    // Regression test for https://github.com/nodejs/node/issues/35730
    {
      function Func() {}
      Func.prototype = null;
      const object = {};
      object.constructor = Func;

      assert.strictEqual(
        util.inspect(object),
        '{ constructor: [Function: Func] }'
      );
    }

    // Test changing util.inspect.colors colors and aliases.
    {
      const colors = util.inspect.colors;

      const originalValue = colors.gray;

      // "grey" is reference-equal alias of "gray".
      assert.strictEqual(colors.grey, colors.gray);

      // Assigninging one should assign the other. This tests that the alias setter
      // function keeps things reference-equal.
      colors.gray = [0, 0];
      assert.deepStrictEqual(colors.gray, [0, 0]);
      assert.strictEqual(colors.grey, colors.gray);

      colors.grey = [1, 1];
      assert.deepStrictEqual(colors.grey, [1, 1]);
      assert.strictEqual(colors.grey, colors.gray);

      // Restore original value to avoid side effects in other tests.
      colors.gray = originalValue;
      assert.deepStrictEqual(colors.gray, originalValue);
      assert.strictEqual(colors.grey, colors.gray);
    }

    // Truncate output for Primitives with 1 character left
    {
      assert.strictEqual(
        util.inspect('bl', { maxStringLength: 1 }),
        "'b'... 1 more character"
      );
    }

    {
      const x = 'a'.repeat(1e6);
      assert(util.inspect(x).endsWith('... 990000 more characters'));
      assert.strictEqual(
        util.inspect(x, { maxStringLength: 4 }),
        "'aaaa'... 999996 more characters"
      );
      assert.match(util.inspect(x, { maxStringLength: null }), /a'$/);
    }

    {
      // Confirm that own constructor value displays correctly.

      function Fhqwhgads() {}

      const sterrance = new Fhqwhgads();
      sterrance.constructor = Fhqwhgads;

      assert.strictEqual(
        util.inspect(sterrance, { showHidden: true }),
        'Fhqwhgads {\n' +
          '  constructor: <ref *1> [Function: Fhqwhgads] {\n' +
          '    [length]: 0,\n' +
          "    [name]: 'Fhqwhgads',\n" +
          '    [prototype]: { [constructor]: [Circular *1] }\n' +
          '  }\n' +
          '}'
      );
    }

    // TODO(soon): figure out why we're generating `Iterator [Generator]` instead of `Object [Generator]`
    {
      // Confirm null prototype of generator prototype displays as expected.

      function getProtoOfProto() {
        return Object.getPrototypeOf(Object.getPrototypeOf(function* () {}));
      }

      function* generator() {}

      const generatorPrototype = Object.getPrototypeOf(generator);
      const originalProtoOfProto = Object.getPrototypeOf(generatorPrototype);
      assert.strictEqual(getProtoOfProto(), originalProtoOfProto);
      Object.setPrototypeOf(generatorPrototype, null);
      assert.notStrictEqual(getProtoOfProto, originalProtoOfProto);

      // This is the actual test. The other assertions in this block are about
      // making sure the test is set up correctly and isn't polluting other tests.
      const expected1 =
        '[GeneratorFunction: generator] {\n' +
        '  [length]: 0,\n' +
        "  [name]: 'generator',\n" +
        "  [prototype]: Object [Generator] { [Symbol(Symbol.toStringTag)]: 'Generator' },\n" + // eslint-disable-line max-len
        "  [Symbol(Symbol.toStringTag)]: 'GeneratorFunction'\n" +
        '}';
      const expected2 = // TODO: Remove this once updated to new V8.
        '[GeneratorFunction: generator] {\n' +
        '  [length]: 0,\n' +
        "  [name]: 'generator',\n" +
        "  [prototype]: Iterator [Generator] { [Symbol(Symbol.toStringTag)]: 'Generator' },\n" + // eslint-disable-line max-len
        "  [Symbol(Symbol.toStringTag)]: 'GeneratorFunction'\n" +
        '}';
      const actual = util.inspect(generator, { showHidden: true });
      assert.ok(actual == expected1 || actual == expected2);

      // Reset so we don't pollute other tests
      Object.setPrototypeOf(generatorPrototype, originalProtoOfProto);
      assert.strictEqual(getProtoOfProto(), originalProtoOfProto);
    }

    {
      // Test for when breakLength results in a single column.
      const obj = Array(9).fill('fhqwhgadshgnsdhjsdbkhsdabkfabkveybvf');
      assert.strictEqual(
        util.inspect(obj, { breakLength: 256 }),
        '[\n' +
          "  'fhqwhgadshgnsdhjsdbkhsdabkfabkveybvf',\n" +
          "  'fhqwhgadshgnsdhjsdbkhsdabkfabkveybvf',\n" +
          "  'fhqwhgadshgnsdhjsdbkhsdabkfabkveybvf',\n" +
          "  'fhqwhgadshgnsdhjsdbkhsdabkfabkveybvf',\n" +
          "  'fhqwhgadshgnsdhjsdbkhsdabkfabkveybvf',\n" +
          "  'fhqwhgadshgnsdhjsdbkhsdabkfabkveybvf',\n" +
          "  'fhqwhgadshgnsdhjsdbkhsdabkfabkveybvf',\n" +
          "  'fhqwhgadshgnsdhjsdbkhsdabkfabkveybvf',\n" +
          "  'fhqwhgadshgnsdhjsdbkhsdabkfabkveybvf'\n" +
          ']'
      );
    }

    {
      assert.strictEqual(
        util.inspect({ ['__proto__']: { a: 1 } }),
        "{ ['__proto__']: { a: 1 } }"
      );
    }

    {
      const { numericSeparator } = util.inspect.defaultOptions;
      util.inspect.defaultOptions.numericSeparator = true;

      assert.strictEqual(
        // eslint-disable-next-line no-loss-of-precision
        util.inspect(1234567891234567891234),
        '1.234567891234568e+21'
      );
      assert.strictEqual(
        util.inspect(123456789.12345678),
        '123_456_789.123_456_78'
      );

      assert.strictEqual(util.inspect(10_000_000), '10_000_000');
      assert.strictEqual(util.inspect(1_000_000), '1_000_000');
      assert.strictEqual(util.inspect(100_000), '100_000');
      assert.strictEqual(util.inspect(99_999.9), '99_999.9');
      assert.strictEqual(util.inspect(9_999), '9_999');
      assert.strictEqual(util.inspect(999), '999');
      assert.strictEqual(util.inspect(NaN), 'NaN');
      assert.strictEqual(util.inspect(Infinity), 'Infinity');
      assert.strictEqual(util.inspect(-Infinity), '-Infinity');

      assert.strictEqual(
        util.inspect(new Float64Array([100_000_000])),
        'Float64Array(1) [ 100_000_000 ]'
      );
      assert.strictEqual(
        util.inspect(new BigInt64Array([9_100_000_100n])),
        'BigInt64Array(1) [ 9_100_000_100n ]'
      );

      assert.strictEqual(util.inspect(123456789), '123_456_789');
      assert.strictEqual(util.inspect(123456789n), '123_456_789n');

      util.inspect.defaultOptions.numericSeparator = numericSeparator;

      assert.strictEqual(
        util.inspect(123456789.12345678, { numericSeparator: true }),
        '123_456_789.123_456_78'
      );

      assert.strictEqual(
        util.inspect(-123456789.12345678, { numericSeparator: true }),
        '-123_456_789.123_456_78'
      );
    }

    // Regression test for https://github.com/nodejs/node/issues/41244
    {
      assert.strictEqual(
        util.inspect({
          get [Symbol.iterator]() {
            throw new Error();
          },
        }),
        '{ [Symbol(Symbol.iterator)]: [Getter] }'
      );
    }

    assertCalledMustCalls();
  },
};

export const utilInspectProxy = {
  // test-util-inspect-proxy.js
  test(ctrl, env, ctx) {
    const opts = { showProxy: true };

    let proxyObj;
    let called = false;
    const target = {
      [util.inspect.custom](depth, { showProxy }) {
        if (showProxy === false) {
          called = true;
          if (proxyObj !== this) {
            throw new Error('Failed');
          }
        }
        return [1, 2, 3];
      },
    };
    const handler = {
      getPrototypeOf() {
        throw new Error('getPrototypeOf');
      },
      setPrototypeOf() {
        throw new Error('setPrototypeOf');
      },
      isExtensible() {
        throw new Error('isExtensible');
      },
      preventExtensions() {
        throw new Error('preventExtensions');
      },
      getOwnPropertyDescriptor() {
        throw new Error('getOwnPropertyDescriptor');
      },
      defineProperty() {
        throw new Error('defineProperty');
      },
      has() {
        throw new Error('has');
      },
      get() {
        throw new Error('get');
      },
      set() {
        throw new Error('set');
      },
      deleteProperty() {
        throw new Error('deleteProperty');
      },
      ownKeys() {
        throw new Error('ownKeys');
      },
      apply() {
        throw new Error('apply');
      },
      construct() {
        throw new Error('construct');
      },
    };
    proxyObj = new Proxy(target, handler);

    // Inspecting the proxy should not actually walk it's properties
    util.inspect(proxyObj, opts);

    // Make sure inspecting object does not trigger any proxy traps.
    util.format('%s', proxyObj);

    const r = Proxy.revocable({}, {});
    r.revoke();

    assert.strictEqual(util.inspect(r.proxy), '<Revoked Proxy>');
    assert.strictEqual(
      util.inspect(r, { showProxy: true }),
      '{ proxy: <Revoked Proxy>, revoke: [Function (anonymous)] }'
    );

    assert.strictEqual(util.format('%s', r.proxy), '<Revoked Proxy>');

    assert.strictEqual(
      util.inspect(proxyObj, opts),
      'Proxy [\n' +
        '  [ 1, 2, 3 ],\n' +
        '  {\n' +
        '    getPrototypeOf: [Function: getPrototypeOf],\n' +
        '    setPrototypeOf: [Function: setPrototypeOf],\n' +
        '    isExtensible: [Function: isExtensible],\n' +
        '    preventExtensions: [Function: preventExtensions],\n' +
        '    getOwnPropertyDescriptor: [Function: getOwnPropertyDescriptor],\n' +
        '    defineProperty: [Function: defineProperty],\n' +
        '    has: [Function: has],\n' +
        '    get: [Function: get],\n' +
        '    set: [Function: set],\n' +
        '    deleteProperty: [Function: deleteProperty],\n' +
        '    ownKeys: [Function: ownKeys],\n' +
        '    apply: [Function: apply],\n' +
        '    construct: [Function: construct]\n' +
        '  }\n' +
        ']'
    );

    // Inspecting a proxy without the showProxy option set to true should not
    // trigger any proxy handlers.
    assert.strictEqual(util.inspect(proxyObj), '[ 1, 2, 3 ]');
    assert(called);

    // Yo dawg, I heard you liked Proxy so I put a Proxy
    // inside your Proxy that proxies your Proxy's Proxy.
    const proxy1 = new Proxy({}, {});
    const proxy2 = new Proxy(proxy1, {});
    const proxy3 = new Proxy(proxy2, proxy1);
    const proxy4 = new Proxy(proxy1, proxy2);
    const proxy5 = new Proxy(proxy3, proxy4);
    const proxy6 = new Proxy(proxy5, proxy5);
    const expected0 = '{}';
    const expected1 = 'Proxy [ {}, {} ]';
    const expected2 = 'Proxy [ Proxy [ {}, {} ], {} ]';
    const expected3 =
      'Proxy [ Proxy [ Proxy [ {}, {} ], {} ], Proxy [ {}, {} ] ]';
    const expected4 =
      'Proxy [ Proxy [ {}, {} ], Proxy [ Proxy [ {}, {} ], {} ] ]';
    const expected5 =
      'Proxy [\n  ' +
      'Proxy [ Proxy [ Proxy [Array], {} ], Proxy [ {}, {} ] ],\n' +
      '  Proxy [ Proxy [ {}, {} ], Proxy [ Proxy [Array], {} ] ]' +
      '\n]';
    const expected6 =
      'Proxy [\n' +
      '  Proxy [\n' +
      '    Proxy [ Proxy [Array], Proxy [Array] ],\n' +
      '    Proxy [ Proxy [Array], Proxy [Array] ]\n' +
      '  ],\n' +
      '  Proxy [\n' +
      '    Proxy [ Proxy [Array], Proxy [Array] ],\n' +
      '    Proxy [ Proxy [Array], Proxy [Array] ]\n' +
      '  ]\n' +
      ']';
    assert.strictEqual(
      util.inspect(proxy1, { showProxy: 1, depth: null }),
      expected1
    );
    assert.strictEqual(util.inspect(proxy2, opts), expected2);
    assert.strictEqual(util.inspect(proxy3, opts), expected3);
    assert.strictEqual(util.inspect(proxy4, opts), expected4);
    assert.strictEqual(util.inspect(proxy5, opts), expected5);
    assert.strictEqual(util.inspect(proxy6, opts), expected6);
    assert.strictEqual(util.inspect(proxy1), expected0);
    assert.strictEqual(util.inspect(proxy2), expected0);
    assert.strictEqual(util.inspect(proxy3), expected0);
    assert.strictEqual(util.inspect(proxy4), expected0);
    assert.strictEqual(util.inspect(proxy5), expected0);
    assert.strictEqual(util.inspect(proxy6), expected0);

    // Just for fun, let's create a Proxy using Arrays.
    const proxy7 = new Proxy([], []);
    const expected7 = 'Proxy [ [], [] ]';
    assert.strictEqual(util.inspect(proxy7, opts), expected7);
    assert.strictEqual(util.inspect(proxy7), '[]');

    // Now we're just getting silly, right?
    const proxy8 = new Proxy(Date, []);
    const proxy9 = new Proxy(Date, String);
    const expected8 = 'Proxy [ [Function: Date], [] ]';
    const expected9 = 'Proxy [ [Function: Date], [Function: String] ]';
    assert.strictEqual(util.inspect(proxy8, opts), expected8);
    assert.strictEqual(util.inspect(proxy9, opts), expected9);
    assert.strictEqual(util.inspect(proxy8), '[Function: Date]');
    assert.strictEqual(util.inspect(proxy9), '[Function: Date]');

    const proxy10 = new Proxy(() => {}, {});
    const proxy11 = new Proxy(() => {}, {
      get() {
        return proxy11;
      },
      apply() {
        return proxy11;
      },
    });
    const expected10 = '[Function (anonymous)]';
    const expected11 = '[Function (anonymous)]';
    assert.strictEqual(util.inspect(proxy10), expected10);
    assert.strictEqual(util.inspect(proxy11), expected11);
  },
};

export const utilInspectGettersAccessingThis = {
  // test-util-inspect-getters-accessing-this.js
  test(ctrl, env, ctx) {
    {
      class X {
        constructor() {
          this._y = 123;
        }

        get y() {
          return this._y;
        }
      }

      const result = inspect(new X(), {
        getters: true,
        showHidden: true,
      });

      assert.strictEqual(result, 'X { _y: 123, [y]: [Getter: 123] }');
    }

    // Regression test for https://github.com/nodejs/node/issues/37054
    {
      class A {
        constructor(B) {
          this.B = B;
        }
        get b() {
          return this.B;
        }
      }

      class B {
        constructor() {
          this.A = new A(this);
        }
        get a() {
          return this.A;
        }
      }

      const result = inspect(new B(), {
        depth: 1,
        getters: true,
        showHidden: true,
      });

      assert.strictEqual(
        result,
        '<ref *1> B {\n' +
          '  A: A { B: [Circular *1], [b]: [Getter] [Circular *1] },\n' +
          '  [a]: [Getter] A { B: [Circular *1], [b]: [Getter] [Circular *1] }\n' +
          '}'
      );
    }
  },
};

export const utilFormat = {
  // test-util-format.js
  async test(ctrl, env, ctx) {
    const symbol = Symbol('foo');

    assert.strictEqual(util.format(), '');
    assert.strictEqual(util.format(''), '');
    assert.strictEqual(util.format([]), '[]');
    assert.strictEqual(util.format([0]), '[ 0 ]');
    assert.strictEqual(util.format({}), '{}');
    assert.strictEqual(util.format({ foo: 42 }), '{ foo: 42 }');
    assert.strictEqual(util.format(null), 'null');
    assert.strictEqual(util.format(true), 'true');
    assert.strictEqual(util.format(false), 'false');
    assert.strictEqual(util.format('test'), 'test');

    // CHECKME this is for console.log() compatibility - but is it *right*?
    assert.strictEqual(util.format('foo', 'bar', 'baz'), 'foo bar baz');

    // ES6 Symbol handling
    assert.strictEqual(util.format(symbol), 'Symbol(foo)');
    assert.strictEqual(util.format('foo', symbol), 'foo Symbol(foo)');
    assert.strictEqual(util.format('%s', symbol), 'Symbol(foo)');
    assert.strictEqual(util.format('%j', symbol), 'undefined');

    // Number format specifier
    assert.strictEqual(util.format('%d'), '%d');
    assert.strictEqual(util.format('%d', 42.0), '42');
    assert.strictEqual(util.format('%d', 42), '42');
    assert.strictEqual(util.format('%d', '42'), '42');
    assert.strictEqual(util.format('%d', '42.0'), '42');
    assert.strictEqual(util.format('%d', 1.5), '1.5');
    assert.strictEqual(util.format('%d', -0.5), '-0.5');
    assert.strictEqual(util.format('%d', -0.0), '-0');
    assert.strictEqual(util.format('%d', ''), '0');
    assert.strictEqual(util.format('%d', ' -0.000'), '-0');
    assert.strictEqual(util.format('%d', Symbol()), 'NaN');
    assert.strictEqual(util.format('%d', Infinity), 'Infinity');
    assert.strictEqual(util.format('%d', -Infinity), '-Infinity');
    assert.strictEqual(util.format('%d %d', 42, 43), '42 43');
    assert.strictEqual(util.format('%d %d', 42), '42 %d');
    assert.strictEqual(
      util.format('%d', 1180591620717411303424),
      '1.1805916207174113e+21'
    );
    assert.strictEqual(
      util.format('%d', 1180591620717411303424n),
      '1180591620717411303424n'
    );
    assert.strictEqual(
      util.format('%d %d', 1180591620717411303424n, 12345678901234567890123n),
      '1180591620717411303424n 12345678901234567890123n'
    );

    {
      const { numericSeparator } = util.inspect.defaultOptions;
      util.inspect.defaultOptions.numericSeparator = true;

      assert.strictEqual(
        util.format('%d', 1180591620717411303424),
        '1.1805916207174113e+21'
      );

      assert.strictEqual(
        util.format(
          // eslint-disable-next-line no-loss-of-precision
          '%d %s %i',
          118059162071741130342,
          118059162071741130342,
          123_123_123
        ),
        '118_059_162_071_741_140_000 118_059_162_071_741_140_000 123_123_123'
      );

      assert.strictEqual(
        util.format(
          '%d %s',
          1_180_591_620_717_411_303_424n,
          12_345_678_901_234_567_890_123n
        ),
        '1_180_591_620_717_411_303_424n 12_345_678_901_234_567_890_123n'
      );

      assert.strictEqual(
        util.format('%i', 1_180_591_620_717_411_303_424n),
        '1_180_591_620_717_411_303_424n'
      );

      util.inspect.defaultOptions.numericSeparator = numericSeparator;
    }
    // Integer format specifier
    assert.strictEqual(util.format('%i'), '%i');
    assert.strictEqual(util.format('%i', 42.0), '42');
    assert.strictEqual(util.format('%i', 42), '42');
    assert.strictEqual(util.format('%i', '42'), '42');
    assert.strictEqual(util.format('%i', '42.0'), '42');
    assert.strictEqual(util.format('%i', 1.5), '1');
    assert.strictEqual(util.format('%i', -0.5), '-0');
    assert.strictEqual(util.format('%i', ''), 'NaN');
    assert.strictEqual(util.format('%i', Infinity), 'NaN');
    assert.strictEqual(util.format('%i', -Infinity), 'NaN');
    assert.strictEqual(util.format('%i', Symbol()), 'NaN');
    assert.strictEqual(util.format('%i %i', 42, 43), '42 43');
    assert.strictEqual(util.format('%i %i', 42), '42 %i');
    assert.strictEqual(util.format('%i', 1180591620717411303424), '1');
    assert.strictEqual(
      util.format('%i', 1180591620717411303424n),
      '1180591620717411303424n'
    );
    assert.strictEqual(
      util.format('%i %i', 1180591620717411303424n, 12345678901234567890123n),
      '1180591620717411303424n 12345678901234567890123n'
    );

    assert.strictEqual(
      util.format('%d %i', 1180591620717411303424n, 12345678901234567890123n),
      '1180591620717411303424n 12345678901234567890123n'
    );

    assert.strictEqual(
      util.format('%i %d', 1180591620717411303424n, 12345678901234567890123n),
      '1180591620717411303424n 12345678901234567890123n'
    );

    assert.strictEqual(
      util.formatWithOptions(
        { numericSeparator: true },
        '%i %d',
        1180591620717411303424n,
        12345678901234567890123n
      ),
      '1_180_591_620_717_411_303_424n 12_345_678_901_234_567_890_123n'
    );

    // Float format specifier
    assert.strictEqual(util.format('%f'), '%f');
    assert.strictEqual(util.format('%f', 42.0), '42');
    assert.strictEqual(util.format('%f', 42), '42');
    assert.strictEqual(util.format('%f', '42'), '42');
    assert.strictEqual(util.format('%f', '-0.0'), '-0');
    assert.strictEqual(util.format('%f', '42.0'), '42');
    assert.strictEqual(util.format('%f', 1.5), '1.5');
    assert.strictEqual(util.format('%f', -0.5), '-0.5');
    assert.strictEqual(util.format('%f', Math.PI), '3.141592653589793');
    assert.strictEqual(util.format('%f', ''), 'NaN');
    assert.strictEqual(util.format('%f', Symbol('foo')), 'NaN');
    assert.strictEqual(util.format('%f', 5n), '5');
    assert.strictEqual(util.format('%f', Infinity), 'Infinity');
    assert.strictEqual(util.format('%f', -Infinity), '-Infinity');
    assert.strictEqual(util.format('%f %f', 42, 43), '42 43');
    assert.strictEqual(util.format('%f %f', 42), '42 %f');

    // String format specifier
    assert.strictEqual(util.format('%s'), '%s');
    assert.strictEqual(util.format('%s', undefined), 'undefined');
    assert.strictEqual(util.format('%s', null), 'null');
    assert.strictEqual(util.format('%s', 'foo'), 'foo');
    assert.strictEqual(util.format('%s', 42), '42');
    assert.strictEqual(util.format('%s', '42'), '42');
    assert.strictEqual(util.format('%s', -0), '-0');
    assert.strictEqual(util.format('%s', '-0.0'), '-0.0');
    assert.strictEqual(util.format('%s %s', 42, 43), '42 43');
    assert.strictEqual(util.format('%s %s', 42), '42 %s');
    assert.strictEqual(util.format('%s', 42n), '42n');
    assert.strictEqual(util.format('%s', Symbol('foo')), 'Symbol(foo)');
    assert.strictEqual(util.format('%s', true), 'true');
    assert.strictEqual(util.format('%s', { a: [1, 2, 3] }), '{ a: [Array] }');
    assert.strictEqual(
      util.format('%s', {
        toString() {
          return 'Foo';
        },
      }),
      'Foo'
    );
    assert.strictEqual(util.format('%s', { toString: 5 }), '{ toString: 5 }');
    assert.strictEqual(
      util.format('%s', () => 5),
      '() => 5'
    );
    assert.strictEqual(util.format('%s', Infinity), 'Infinity');
    assert.strictEqual(util.format('%s', -Infinity), '-Infinity');

    // String format specifier including `toString` properties on the prototype.
    {
      class Foo {
        toString() {
          return 'Bar';
        }
      }
      assert.strictEqual(util.format('%s', new Foo()), 'Bar');
      assert.strictEqual(
        util.format('%s', Object.setPrototypeOf(new Foo(), null)),
        '[Foo: null prototype] {}'
      );
      globalThis.Foo = Foo;
      assert.strictEqual(util.format('%s', new Foo()), 'Bar');
      delete globalThis.Foo;
      class Bar {
        abc = true;
      }
      assert.strictEqual(util.format('%s', new Bar()), 'Bar { abc: true }');
      class Foobar extends Array {
        aaa = true;
      }
      assert.strictEqual(
        util.format('%s', new Foobar(5)),
        'Foobar(5) [ <5 empty items>, aaa: true ]'
      );

      // Subclassing:
      class B extends Foo {}

      function C() {}
      C.prototype.toString = function () {
        return 'Custom';
      };

      function D() {
        C.call(this);
      }
      D.prototype = { __proto__: C.prototype };

      assert.strictEqual(util.format('%s', new B()), 'Bar');
      assert.strictEqual(util.format('%s', new C()), 'Custom');
      assert.strictEqual(util.format('%s', new D()), 'Custom');

      D.prototype.constructor = D;
      assert.strictEqual(util.format('%s', new D()), 'Custom');

      D.prototype.constructor = null;
      assert.strictEqual(util.format('%s', new D()), 'Custom');

      D.prototype.constructor = { name: 'Foobar' };
      assert.strictEqual(util.format('%s', new D()), 'Custom');

      Object.defineProperty(D.prototype, 'constructor', {
        get() {
          throw new Error();
        },
        configurable: true,
      });
      assert.strictEqual(util.format('%s', new D()), 'Custom');

      assert.strictEqual(
        util.format('%s', { __proto__: null }),
        '[Object: null prototype] {}'
      );
    }

    // JSON format specifier
    assert.strictEqual(util.format('%j'), '%j');
    assert.strictEqual(util.format('%j', 42), '42');
    assert.strictEqual(util.format('%j', '42'), '"42"');
    assert.strictEqual(util.format('%j %j', 42, 43), '42 43');
    assert.strictEqual(util.format('%j %j', 42), '42 %j');

    // Object format specifier
    const obj = {
      foo: 'bar',
      foobar: 1,
      func: function () {},
    };
    const nestedObj = {
      foo: 'bar',
      foobar: {
        foo: 'bar',
        func: function () {},
      },
    };
    const nestedObj2 = {
      foo: 'bar',
      foobar: 1,
      func: [{ a: function () {} }],
    };
    assert.strictEqual(util.format('%o'), '%o');
    assert.strictEqual(util.format('%o', 42), '42');
    assert.strictEqual(util.format('%o', 'foo'), "'foo'");
    assert.strictEqual(
      util.format('%o', obj),
      '{\n' +
        "  foo: 'bar',\n" +
        '  foobar: 1,\n' +
        '  func: <ref *1> [Function: func] {\n' +
        '    [length]: 0,\n' +
        "    [name]: 'func',\n" +
        '    [prototype]: { [constructor]: [Circular *1] }\n' +
        '  }\n' +
        '}'
    );
    assert.strictEqual(
      util.format('%o', nestedObj2),
      '{\n' +
        "  foo: 'bar',\n" +
        '  foobar: 1,\n' +
        '  func: [\n' +
        '    {\n' +
        '      a: <ref *1> [Function: a] {\n' +
        '        [length]: 0,\n' +
        "        [name]: 'a',\n" +
        '        [prototype]: { [constructor]: [Circular *1] }\n' +
        '      }\n' +
        '    },\n' +
        '    [length]: 1\n' +
        '  ]\n' +
        '}'
    );
    assert.strictEqual(
      util.format('%o', nestedObj),
      '{\n' +
        "  foo: 'bar',\n" +
        '  foobar: {\n' +
        "    foo: 'bar',\n" +
        '    func: <ref *1> [Function: func] {\n' +
        '      [length]: 0,\n' +
        "      [name]: 'func',\n" +
        '      [prototype]: { [constructor]: [Circular *1] }\n' +
        '    }\n' +
        '  }\n' +
        '}'
    );
    assert.strictEqual(
      util.format('%o %o', obj, obj),
      '{\n' +
        "  foo: 'bar',\n" +
        '  foobar: 1,\n' +
        '  func: <ref *1> [Function: func] {\n' +
        '    [length]: 0,\n' +
        "    [name]: 'func',\n" +
        '    [prototype]: { [constructor]: [Circular *1] }\n' +
        '  }\n' +
        '} {\n' +
        "  foo: 'bar',\n" +
        '  foobar: 1,\n' +
        '  func: <ref *1> [Function: func] {\n' +
        '    [length]: 0,\n' +
        "    [name]: 'func',\n" +
        '    [prototype]: { [constructor]: [Circular *1] }\n' +
        '  }\n' +
        '}'
    );
    assert.strictEqual(
      util.format('%o %o', obj),
      '{\n' +
        "  foo: 'bar',\n" +
        '  foobar: 1,\n' +
        '  func: <ref *1> [Function: func] {\n' +
        '    [length]: 0,\n' +
        "    [name]: 'func',\n" +
        '    [prototype]: { [constructor]: [Circular *1] }\n' +
        '  }\n' +
        '} %o'
    );

    assert.strictEqual(util.format('%O'), '%O');
    assert.strictEqual(util.format('%O', 42), '42');
    assert.strictEqual(util.format('%O', 'foo'), "'foo'");
    assert.strictEqual(
      util.format('%O', obj),
      "{ foo: 'bar', foobar: 1, func: [Function: func] }"
    );
    assert.strictEqual(
      util.format('%O', nestedObj),
      "{ foo: 'bar', foobar: { foo: 'bar', func: [Function: func] } }"
    );
    assert.strictEqual(
      util.format('%O %O', obj, obj),
      "{ foo: 'bar', foobar: 1, func: [Function: func] } " +
        "{ foo: 'bar', foobar: 1, func: [Function: func] }"
    );
    assert.strictEqual(
      util.format('%O %O', obj),
      "{ foo: 'bar', foobar: 1, func: [Function: func] } %O"
    );

    // Various format specifiers
    assert.strictEqual(util.format('%%s%s', 'foo'), '%sfoo');
    assert.strictEqual(util.format('%s:%s'), '%s:%s');
    assert.strictEqual(util.format('%s:%s', undefined), 'undefined:%s');
    assert.strictEqual(util.format('%s:%s', 'foo'), 'foo:%s');
    assert.strictEqual(util.format('%s:%i', 'foo'), 'foo:%i');
    assert.strictEqual(util.format('%s:%f', 'foo'), 'foo:%f');
    assert.strictEqual(util.format('%s:%s', 'foo', 'bar'), 'foo:bar');
    assert.strictEqual(
      util.format('%s:%s', 'foo', 'bar', 'baz'),
      'foo:bar baz'
    );
    assert.strictEqual(util.format('%%%s%%', 'hi'), '%hi%');
    assert.strictEqual(util.format('%%%s%%%%', 'hi'), '%hi%%');
    assert.strictEqual(util.format('%sbc%%def', 'a'), 'abc%def');
    assert.strictEqual(util.format('%d:%d', 12, 30), '12:30');
    assert.strictEqual(util.format('%d:%d', 12), '12:%d');
    assert.strictEqual(util.format('%d:%d'), '%d:%d');
    assert.strictEqual(util.format('%i:%i', 12, 30), '12:30');
    assert.strictEqual(util.format('%i:%i', 12), '12:%i');
    assert.strictEqual(util.format('%i:%i'), '%i:%i');
    assert.strictEqual(util.format('%f:%f', 12, 30), '12:30');
    assert.strictEqual(util.format('%f:%f', 12), '12:%f');
    assert.strictEqual(util.format('%f:%f'), '%f:%f');
    assert.strictEqual(util.format('o: %j, a: %j', {}, []), 'o: {}, a: []');
    assert.strictEqual(util.format('o: %j, a: %j', {}), 'o: {}, a: %j');
    assert.strictEqual(util.format('o: %j, a: %j'), 'o: %j, a: %j');
    assert.strictEqual(util.format('o: %o, a: %O', {}, []), 'o: {}, a: []');
    assert.strictEqual(util.format('o: %o, a: %o', {}), 'o: {}, a: %o');
    assert.strictEqual(util.format('o: %O, a: %O'), 'o: %O, a: %O');

    // Invalid format specifiers
    assert.strictEqual(util.format('a% b', 'x'), 'a% b x');
    assert.strictEqual(
      util.format('percent: %d%, fraction: %d', 10, 0.1),
      'percent: 10%, fraction: 0.1'
    );
    assert.strictEqual(util.format('abc%', 1), 'abc% 1');

    // Additional arguments after format specifiers
    assert.strictEqual(util.format('%i', 1, 'number'), '1 number');
    assert.strictEqual(
      util.format('%i', 1, () => {}),
      '1 [Function (anonymous)]'
    );

    // %c from https://console.spec.whatwg.org/
    assert.strictEqual(util.format('%c'), '%c');
    assert.strictEqual(util.format('%cab'), '%cab');
    assert.strictEqual(util.format('%cab', 'color: blue'), 'ab');
    assert.strictEqual(util.format('%cab', 'color: blue', 'c'), 'ab c');

    {
      const o = {};
      o.o = o;
      assert.strictEqual(util.format('%j', o), '[Circular]');
    }

    {
      const o = {
        toJSON() {
          throw new Error('Not a circular object but still not serializable');
        },
      };
      assert.throws(
        () => util.format('%j', o),
        /^Error: Not a circular object but still not serializable$/
      );
    }

    // Errors
    const err = new Error('foo');
    assert.strictEqual(util.format(err), err.stack);
    class CustomError extends Error {
      constructor(msg) {
        super();
        Object.defineProperty(this, 'message', {
          value: msg,
          enumerable: false,
        });
        Object.defineProperty(this, 'name', {
          value: 'CustomError',
          enumerable: false,
        });
        Error.captureStackTrace(this, CustomError);
      }
    }
    const customError = new CustomError('bar');
    assert.strictEqual(util.format(customError), customError.stack);
    // Doesn't capture stack trace
    function BadCustomError(msg) {
      Error.call(this);
      Object.defineProperty(this, 'message', { value: msg, enumerable: false });
      Object.defineProperty(this, 'name', {
        value: 'BadCustomError',
        enumerable: false,
      });
    }
    Object.setPrototypeOf(BadCustomError.prototype, Error.prototype);
    Object.setPrototypeOf(BadCustomError, Error);
    assert.strictEqual(
      util.format(new BadCustomError('foo')),
      '[BadCustomError: foo]'
    );

    // The format of arguments should not depend on type of the first argument
    assert.strictEqual(util.format('1', '1'), '1 1');
    assert.strictEqual(util.format(1, '1'), '1 1');
    assert.strictEqual(util.format('1', 1), '1 1');
    assert.strictEqual(util.format(1, -0), '1 -0');
    assert.strictEqual(
      util.format('1', () => {}),
      '1 [Function (anonymous)]'
    );
    assert.strictEqual(
      util.format(1, () => {}),
      '1 [Function (anonymous)]'
    );
    assert.strictEqual(util.format('1', "'"), "1 '");
    assert.strictEqual(util.format(1, "'"), "1 '");
    assert.strictEqual(util.format('1', 'number'), '1 number');
    assert.strictEqual(util.format(1, 'number'), '1 number');
    assert.strictEqual(util.format(5n), '5n');
    assert.strictEqual(util.format(5n, 5n), '5n 5n');

    // Check `formatWithOptions`.
    assert.strictEqual(
      util.formatWithOptions(
        { colors: true },
        true,
        undefined,
        Symbol(),
        1,
        5n,
        null,
        'foobar'
      ),
      '\u001b[33mtrue\u001b[39m ' +
        '\u001b[90mundefined\u001b[39m ' +
        '\u001b[32mSymbol()\u001b[39m ' +
        '\u001b[33m1\u001b[39m ' +
        '\u001b[33m5n\u001b[39m ' +
        '\u001b[1mnull\u001b[22m ' +
        'foobar'
    );

    assert.strictEqual(
      util.format(new SharedArrayBuffer(4)),
      'SharedArrayBuffer { [Uint8Contents]: <00 00 00 00>, byteLength: 4 }'
    );

    assert.strictEqual(
      util.formatWithOptions({ colors: true, compact: 3 }, '%s', [
        1,
        { a: true },
      ]),
      '[ 1, [Object] ]'
    );

    [undefined, null, false, 5n, 5, 'test', Symbol()].forEach(
      (invalidOptions) => {
        assert.throws(
          () => {
            util.formatWithOptions(invalidOptions, { a: true });
          },
          {
            code: 'ERR_INVALID_ARG_TYPE',
            message: /"inspectOptions".+object/,
          }
        );
      }
    );
  },
};

export const utilInspectError = {
  // message/util_inspect_error.js
  async test(ctrl, env, ctx) {
    const err = new Error('foo\nbar');

    assert.match(
      util.inspect({ err, nested: { err } }, { compact: true }),
      /{ err: Error: foo\n   bar\n       at .+,\n  nested: { err: Error: foo\n      bar\n          at .+ } }/s
    );
    assert.match(
      util.inspect({ err, nested: { err } }, { compact: false }),
      /{\n  err: Error: foo\n  bar\n      at .+,\n  nested: {\n    err: Error: foo\n    bar\n        at .+\n  }\n}/s
    );

    err.foo = 'bar';
    assert.match(
      util.inspect(err, { compact: true, breakLength: 5 }),
      /{ Error: foo\nbar\n    at .+\n  foo: 'bar' }/
    );
  },
};

export const logTest = {
  test() {
    const original = console.log;
    console.log = mock.fn();

    util.log('test');

    assert.strictEqual(console.log.mock.callCount(), 1);
    const args = console.log.mock.calls[0].arguments;
    assert.strictEqual(args.length, 3);
    assert.strictEqual(args[0], '%s - %s');
    // skipping the check on args[1] since it'll be a timestamp that changes
    assert.strictEqual(args[2], 'test');

    console.log = original;
  },
};

export const aborted = {
  async test() {
    const signal = AbortSignal.timeout(10);
    await util.aborted(signal, {});

    await assert.rejects(util.aborted({}, {}), {
      message:
        'The "signal" argument must be an instance of AbortSignal. ' +
        'Received an instance of Object',
    });
  },
};

export const debuglog = {
  test() {
    const original = console.log;
    console.log = mock.fn();

    util.debuglog('test')('hello');

    assert.strictEqual(console.log.mock.callCount(), 1);
    const args = console.log.mock.calls[0].arguments;
    assert.strictEqual(args.length, 1);
    assert.strictEqual(args[0], 'TEST: hello\n');

    console.log = original;
  },
};

export const getCallSiteTest = {
  test() {
    const callSites = util.getCallSite();
    assert.strictEqual(callSites.length, 1);
    const [stack] = callSites;
    assert.strictEqual(stack.functionName, 'test');
    assert.strictEqual(stack.scriptName, 'worker');
    assert.strictEqual(typeof stack.lineNumber, 'number');
    assert.strictEqual(typeof stack.column, 'number');
  },
};

export const isDeepStrictEqual = {
  test() {
    util.isDeepStrictEqual(1, 1);
    util.isDeepStrictEqual(['hello', 'world'], ['hello', 'world']);
  },
};

export const isArray = {
  test() {
    assert.ok(util.isArray([]));
    assert.ok(!util.isArray('hello world'));
  },
};

export const makeSureUtilTypesIsExported = {
  async test() {
    const types = await import('node:util/types');
    assert.ok(types, 'node:util/types is not exported');
    assert.ok(types.isTypedArray);
    assert.ok(types.default.isTypedArray);
  },
};
