// Copyright (c) 2025 Cloudflare, Inc.
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
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

import consoleModule, { Console } from 'node:console';
import {
  deepStrictEqual,
  doesNotThrow,
  match,
  ok,
  strictEqual,
  throws,
} from 'node:assert';
import { inspect } from 'node:util';
import { Writable } from 'node:stream';

// These methods are retrieved using Object.keys(require('node:console'))
// on Node.js v22.17.1
const methods = [
  'log',
  'info',
  'debug',
  'warn',
  'error',
  'dir',
  'time',
  'timeEnd',
  'timeLog',
  'trace',
  'assert',
  'clear',
  'count',
  'countReset',
  'group',
  'groupEnd',
  'table',
  'dirxml',
  'groupCollapsed',
  'Console',
  'profile',
  'profileEnd',
  'timeStamp',
  'context',
  'createTask',
];

const notImplementedMethods = ['context', 'createTask'];

export const testMethodNames = {
  async test() {
    for (const method of methods) {
      strictEqual(
        typeof consoleModule[method],
        'function',
        `Method Console.${method} is not a function`
      );
    }
  },
};

export const testNotImplemented = {
  async test() {
    for (const method of notImplementedMethods) {
      throws(() => consoleModule[method](), {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      });
    }
  },
};

export const testMethods = {
  async test() {
    const implementedMethods = methods.filter(
      (m) => !notImplementedMethods.includes(m) && m !== 'Console'
    );
    for (const method of implementedMethods) {
      doesNotThrow(() => consoleModule[method](`Calling Console.${method}()`));
    }
  },
};

// A Writable that records what the console writes to it.
function capture() {
  const out = {
    data: '',
    stream: new Writable({
      write(chunk, _encoding, cb) {
        out.data += chunk;
        cb();
      },
      decodeStrings: false,
    }),
    take() {
      const data = out.data;
      out.data = '';
      return data;
    },
  };
  return out;
}

// From Node.js test/parallel/test-console-instance.js
export const testConsoleInstance = {
  async test() {
    strictEqual(typeof Console, 'function');
    strictEqual(consoleModule, globalThis.console);
    strictEqual(consoleModule.Console, Console);
    ok(globalThis.console instanceof Console);
    ok(!({} instanceof Console));

    throws(() => new Console(), {
      code: 'ERR_CONSOLE_WRITABLE_STREAM',
      name: 'TypeError',
      message: /stdout/,
    });
    throws(() => new Console({ write() {} }, { write: undefined }), {
      code: 'ERR_CONSOLE_WRITABLE_STREAM',
      name: 'TypeError',
      message: /stderr/,
    });

    const out = capture();
    const err = capture();
    const c = new Console(out.stream, err.stream);
    ok(c instanceof Console);

    c.log('test');
    c.error('test');
    strictEqual(out.take(), 'test\n');
    strictEqual(err.take(), 'test\n');

    c.dir({ foo: 1 });
    strictEqual(out.take(), '{ foo: 1 }\n');

    // The methods are bound to the instance.
    [1, 2, 3].forEach(c.log);
    strictEqual(
      out.take(),
      '1 0 [ 1, 2, 3 ]\n2 1 [ 1, 2, 3 ]\n3 2 [ 1, 2, 3 ]\n'
    );

    // Calling Console without `new`.
    ok(Console(out.stream, err.stream) instanceof Console);

    // Extending Console.
    class MyConsole extends Console {
      hello() {}
      log() {
        return 'overridden';
      }
    }
    const myConsole = new MyConsole(out.stream);
    strictEqual(typeof myConsole.hello, 'function');
    ok(myConsole instanceof Console);
    strictEqual(myConsole.log(), 'overridden');
    const log = myConsole.log;
    strictEqual(log(), 'overridden');

    // An instance that does not ignore stream errors.
    const c2 = new Console(
      {
        write() {
          throw new Error('out');
        },
      },
      {
        write() {
          throw new Error('err');
        },
      },
      false
    );
    throws(() => c2.log('foo'), /^Error: out$/);
    throws(() => c2.warn('foo'), /^Error: err$/);
    throws(() => c2.dir('foo'), /^Error: out$/);

    for (const inspectOptions of [null, true, false, 'foo', 5, Symbol()]) {
      throws(
        () =>
          new Console({
            stdout: out.stream,
            stderr: err.stream,
            inspectOptions,
          }),
        {
          code: 'ERR_INVALID_ARG_TYPE',
          message:
            /^The "options\.inspectOptions" property must be of type object\./,
        }
      );
    }
    strictEqual(out.data + err.data, '');
  },
};

// From Node.js test/parallel/test-console-methods.js
export const testConsoleMethodNames = {
  async test() {
    const { stream } = capture();
    const instance = new Console(stream);
    const alternateNames = {
      debug: 'log',
      info: 'log',
      dirxml: 'log',
      error: 'warn',
      groupCollapsed: 'group',
    };
    for (const method of [
      'log',
      'warn',
      'dir',
      'time',
      'timeEnd',
      'timeLog',
      'trace',
      'assert',
      'clear',
      'count',
      'countReset',
      'group',
      'groupEnd',
      'table',
      'debug',
      'info',
      'dirxml',
      'error',
      'groupCollapsed',
    ]) {
      strictEqual(instance[method].name, method);
      ok(
        [method, alternateNames[method]].includes(
          Console.prototype[method].name
        )
      );
      throws(() => new instance[method](), TypeError);
      throws(() => Reflect.construct({}, [], instance[method]), TypeError);
    }
  },
};

// From Node.js test/parallel/test-console-group.js
export const testConsoleGroup = {
  async test() {
    const out = capture();
    const err = capture();
    const setup = (groupIndentation) =>
      new Console({
        stdout: out.stream,
        stderr: err.stream,
        colorMode: false,
        groupIndentation,
      });

    // Basic group() functionality.
    let c = setup();
    c.log('This is the outer level');
    c.group();
    c.log('Level 2');
    c.group();
    c.log('Level 3');
    c.warn('More of level 3');
    c.groupEnd();
    c.log('Back to level 2');
    c.groupEnd();
    c.log('Back to the outer level');
    c.groupEnd();
    c.log('Still at the outer level');
    strictEqual(
      out.take(),
      'This is the outer level\n' +
        '  Level 2\n' +
        '    Level 3\n' +
        '  Back to level 2\n' +
        'Back to the outer level\n' +
        'Still at the outer level\n'
    );
    strictEqual(err.take(), '    More of level 3\n');

    // Group indentation is tracked per Console instance.
    c = setup();
    const c2 = new Console(out.stream, err.stream);
    c.log('No indentation');
    c2.log('None here either');
    c.group();
    c.log('Now the first console is indenting');
    c2.log('But the second one does not');
    strictEqual(
      out.take(),
      'No indentation\n' +
        'None here either\n' +
        '  Now the first console is indenting\n' +
        'But the second one does not\n'
    );

    // Labels.
    c = setup();
    c.group('This is a label');
    c.log('And this is the data for that label');
    strictEqual(
      out.take(),
      'This is a label\n  And this is the data for that label\n'
    );

    // groupCollapsed() is an alias of group().
    c = setup();
    c.groupCollapsed('Label');
    c.log('Level 2');
    c.groupCollapsed();
    c.log('Level 3');
    strictEqual(out.take(), 'Label\n  Level 2\n    Level 3\n');

    // Multiline strings and objects are indented.
    c = setup();
    c.log('not indented');
    c.group();
    c.log('indented\nalso indented');
    c.log({
      also: 'a',
      multiline: 'object',
      should: 'be',
      indented: 'properly',
      kthx: 'bai',
    });
    strictEqual(
      out.take(),
      'not indented\n' +
        '  indented\n' +
        '  also indented\n' +
        '  {\n' +
        "    also: 'a',\n" +
        "    multiline: 'object',\n" +
        "    should: 'be',\n" +
        "    indented: 'properly',\n" +
        "    kthx: 'bai'\n" +
        '  }\n'
    );

    // Custom groupIndentation.
    c = setup(3);
    c.log('This is the outer level');
    c.group();
    c.log('Level 2');
    c.group();
    c.log('Level 3');
    c.warn('More of level 3');
    strictEqual(
      out.take(),
      'This is the outer level\n   Level 2\n      Level 3\n'
    );
    strictEqual(err.take(), '      More of level 3\n');

    for (const e of [null, 'str', [], false, true, {}]) {
      throws(() => setup(e), {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
      });
    }
    for (const e of [NaN, 1.01]) {
      throws(() => setup(e), {
        code: 'ERR_OUT_OF_RANGE',
        name: 'RangeError',
        message: /an integer/,
      });
    }
    for (const e of [-1, 1001]) {
      throws(() => setup(e), {
        code: 'ERR_OUT_OF_RANGE',
        name: 'RangeError',
        message: />= 0 && <= 1000/,
      });
    }
  },
};

// From Node.js test/parallel/test-console-table.js
export const testConsoleTable = {
  async test() {
    const queue = [];
    const c = new Console(
      {
        write: (x) => {
          queue.push(x);
        },
        removeListener: () => {},
      },
      { write() {} },
      false
    );

    function test(data, only, expected) {
      if (arguments.length === 2) {
        expected = only;
        only = undefined;
      }
      c.table(data, only);
      deepStrictEqual(
        queue.shift().split('\n'),
        expected.trimStart().split('\n')
      );
    }

    throws(() => c.table([], false), { code: 'ERR_INVALID_ARG_TYPE' });

    test(null, 'null\n');
    test(undefined, 'undefined\n');
    test(false, 'false\n');
    test('hi', 'hi\n');
    test(Symbol(), 'Symbol()\n');
    test(function () {}, '[Function (anonymous)]\n');

    test(
      [1, 2, 3],
      `
┌─────────┬────────┐
│ (index) │ Values │
├─────────┼────────┤
│ 0       │ 1      │
│ 1       │ 2      │
│ 2       │ 3      │
└─────────┴────────┘
`
    );

    test(
      [Symbol(), 5, [10]],
      `
┌─────────┬────┬──────────┐
│ (index) │ 0  │ Values   │
├─────────┼────┼──────────┤
│ 0       │    │ Symbol() │
│ 1       │    │ 5        │
│ 2       │ 10 │          │
└─────────┴────┴──────────┘
`
    );

    test(
      [undefined, 5],
      `
┌─────────┬───────────┐
│ (index) │ Values    │
├─────────┼───────────┤
│ 0       │ undefined │
│ 1       │ 5         │
└─────────┴───────────┘
`
    );

    test(
      { a: 1, b: Symbol(), c: [10] },
      `
┌─────────┬────┬──────────┐
│ (index) │ 0  │ Values   │
├─────────┼────┼──────────┤
│ a       │    │ 1        │
│ b       │    │ Symbol() │
│ c       │ 10 │          │
└─────────┴────┴──────────┘
`
    );

    test(
      new Map([
        ['a', 1],
        [Symbol(), [2]],
      ]),
      `
┌───────────────────┬──────────┬────────┐
│ (iteration index) │ Key      │ Values │
├───────────────────┼──────────┼────────┤
│ 0                 │ 'a'      │ 1      │
│ 1                 │ Symbol() │ [ 2 ]  │
└───────────────────┴──────────┴────────┘
`
    );

    test(
      new Set([1, 2, Symbol()]),
      `
┌───────────────────┬──────────┐
│ (iteration index) │ Values   │
├───────────────────┼──────────┤
│ 0                 │ 1        │
│ 1                 │ 2        │
│ 2                 │ Symbol() │
└───────────────────┴──────────┘
`
    );

    test(
      { a: 1, b: 2 },
      ['a'],
      `
┌─────────┬───┐
│ (index) │ a │
├─────────┼───┤
│ a       │   │
│ b       │   │
└─────────┴───┘
`
    );

    test(
      [
        { a: 1, b: 2 },
        { a: 3, c: 4 },
      ],
      ['a'],
      `
┌─────────┬───┐
│ (index) │ a │
├─────────┼───┤
│ 0       │ 1 │
│ 1       │ 3 │
└─────────┴───┘
`
    );

    test(
      new Map([
        [1, 1],
        [2, 2],
        [3, 3],
      ]).entries(),
      `
┌───────────────────┬─────┬────────┐
│ (iteration index) │ Key │ Values │
├───────────────────┼─────┼────────┤
│ 0                 │ 1   │ 1      │
│ 1                 │ 2   │ 2      │
│ 2                 │ 3   │ 3      │
└───────────────────┴─────┴────────┘
`
    );

    const iterValues = `
┌───────────────────┬────────┐
│ (iteration index) │ Values │
├───────────────────┼────────┤
│ 0                 │ 1      │
│ 1                 │ 2      │
│ 2                 │ 3      │
└───────────────────┴────────┘
`;
    const map = new Map([
      [1, 1],
      [2, 2],
      [3, 3],
    ]);
    test(map.values(), iterValues);
    test(map.keys(), iterValues);
    test(new Set([1, 2, 3]).values(), iterValues);

    test(
      { a: { a: 1, b: 2, c: 3 } },
      `
┌─────────┬───┬───┬───┐
│ (index) │ a │ b │ c │
├─────────┼───┼───┼───┤
│ a       │ 1 │ 2 │ 3 │
└─────────┴───┴───┴───┘
`
    );

    test(
      { a: { a: { a: 1, b: 2, c: 3 } } },
      `
┌─────────┬──────────┐
│ (index) │ a        │
├─────────┼──────────┤
│ a       │ [Object] │
└─────────┴──────────┘
`
    );

    test(
      { a: [1, 2, 3, 4, 5], b: 5, c: { e: 5 } },
      `
┌─────────┬───┬───┬───┬───┬───┬───┬────────┐
│ (index) │ 0 │ 1 │ 2 │ 3 │ 4 │ e │ Values │
├─────────┼───┼───┼───┼───┼───┼───┼────────┤
│ a       │ 1 │ 2 │ 3 │ 4 │ 5 │   │        │
│ b       │   │   │   │   │   │   │ 5      │
│ c       │   │   │   │   │   │ 5 │        │
└─────────┴───┴───┴───┴───┴───┴───┴────────┘
`
    );

    const indexValues = `
┌─────────┬────────┐
│ (index) │ Values │
├─────────┼────────┤
│ 0       │ 1      │
│ 1       │ 2      │
│ 2       │ 3      │
└─────────┴────────┘
`;
    test(new Uint8Array([1, 2, 3]), indexValues);
    test(Buffer.from([1, 2, 3]), indexValues);

    test(
      { a: undefined },
      ['x'],
      `
┌─────────┬───┐
│ (index) │ x │
├─────────┼───┤
│ a       │   │
└─────────┴───┘
`
    );

    test(
      [],
      `
┌─────────┐
│ (index) │
├─────────┤
└─────────┘
`
    );

    test(
      new Map(),
      `
┌───────────────────┬─────┬────────┐
│ (iteration index) │ Key │ Values │
├───────────────────┼─────┼────────┤
└───────────────────┴─────┴────────┘
`
    );

    test(
      [
        { a: 1, b: 'Y' },
        { a: 'Z', b: 2 },
      ],
      `
┌─────────┬─────┬─────┐
│ (index) │ a   │ b   │
├─────────┼─────┼─────┤
│ 0       │ 1   │ 'Y' │
│ 1       │ 'Z' │ 2   │
└─────────┴─────┴─────┘
`
    );

    test(
      { foo: '￥', bar: '¥' },
      `
┌─────────┬────────┐
│ (index) │ Values │
├─────────┼────────┤
│ foo     │ '￥'   │
│ bar     │ '¥'    │
└─────────┴────────┘
`
    );

    test(
      [{ foo: 10 }, { foo: 20 }],
      ['__proto__'],
      `
┌─────────┬───────────┐
│ (index) │ __proto__ │
├─────────┼───────────┤
│ 0       │           │
│ 1       │           │
└─────────┴───────────┘
`
    );
    strictEqual('0' in Object.prototype, false);
    strictEqual('1' in Object.prototype, false);
  },
};

// From Node.js test/parallel/test-console-tty-colors.js
export const testConsoleColorMode = {
  async test() {
    function check(isTTY, colorMode, expectedColorMode, inspectOptions) {
      const items = [1, { a: 2 }, ['foo'], { '\\a': '\\bar' }];
      const out = capture();
      out.stream.isTTY = isTTY;
      const c = new Console({
        stdout: out.stream,
        ignoreErrors: false,
        colorMode,
        inspectOptions,
      });
      for (const item of items) {
        c.log(item);
        strictEqual(
          out.take().trim(),
          inspect(item, { colors: expectedColorMode, ...inspectOptions })
        );
      }
    }

    check(true, 'auto', true);
    check(false, 'auto', false);
    check(false, undefined, true, { colors: true, compact: false });
    check(true, 'auto', true, { compact: false });
    check(true, undefined, false, { colors: false });
    check(true, true, true);
    check(false, true, true);
    check(true, false, false);
    check(false, false, false);

    const { stream } = capture();
    for (const colorMode of [0, 'true', null, {}, [], () => {}]) {
      throws(
        () => new Console({ stdout: stream, ignoreErrors: false, colorMode }),
        {
          message:
            "The argument 'colorMode' must be one of: 'auto', true, false. " +
            `Received ${inspect(colorMode)}`,
          code: 'ERR_INVALID_ARG_VALUE',
        }
      );
    }
    for (const colorMode of [true, false, 'auto']) {
      throws(
        () =>
          new Console({
            stdout: stream,
            ignoreErrors: false,
            colorMode,
            inspectOptions: { colors: false },
          }),
        { code: 'ERR_INCOMPATIBLE_OPTION_PAIR' }
      );
    }
  },
};

// From Node.js test/parallel/test-console-count.js
export const testConsoleCount = {
  async test() {
    const out = capture();
    const c = new Console(out.stream);
    const expectCount = (label, expected) => {
      c.count(label);
      strictEqual(out.take(), expected);
    };
    expectCount(undefined, 'default: 1\n');
    expectCount('default', 'default: 2\n');
    expectCount('a', 'a: 1\n');
    expectCount('b', 'b: 1\n');
    expectCount('a', 'a: 2\n');
    expectCount(undefined, 'default: 3\n');
    expectCount({}, '[object Object]: 1\n');
    expectCount(1, '1: 1\n');
    expectCount(null, 'null: 1\n');
    expectCount('null', 'null: 2\n');
    c.countReset();
    expectCount(undefined, 'default: 1\n');
    c.countReset('a');
    expectCount('a', 'a: 1\n');
    expectCount(undefined, 'default: 2\n');
    throws(() => c.count(Symbol('test')), TypeError);
    throws(() => c.countReset(Symbol('test')), TypeError);
  },
};

// Behaviour described in Node.js doc/api/console.md for the remaining methods.
export const testConsoleOtherMethods = {
  async test() {
    const out = capture();
    const err = capture();
    const c = new Console({ stdout: out.stream, stderr: err.stream });

    c.info('a %d', 1);
    c.debug('b');
    c.dirxml('c', 2);
    strictEqual(out.take(), 'a 1\nb\nc 2\n');

    c.assert(true, 'nothing');
    c.assert(false, 'x %s', 'y');
    c.assert(false, { a: 1 });
    c.assert(false);
    strictEqual(
      err.take(),
      'Assertion failed: x y\nAssertion failed { a: 1 }\nAssertion failed\n'
    );

    c.trace('here %d', 1);
    match(err.take(), /^Trace: here 1\n {4}at /);

    c.dir({ a: { b: { c: { d: 1 } } } }, { depth: 0 });
    strictEqual(out.take(), '{ a: [Object] }\n');

    c.time('t');
    c.timeLog('t', 'extra', 1);
    match(out.take(), /^t: \d+(\.\d+)?ms extra 1\n$/);
    c.timeEnd('t');
    match(out.take(), /^t: \d+(\.\d+)?ms\n$/);
    c.time();
    c.timeEnd();
    match(out.take(), /^default: \d+(\.\d+)?ms\n$/);

    // clear() only writes to a TTY.
    c.clear();
    strictEqual(out.take(), '');

    strictEqual(Object.prototype.toString.call(c), '[object console]');
  },
};

export const testConsoleIgnoreErrors = {
  async test() {
    const syncThrow = () =>
      new Writable({
        write() {
          throw new Error('sync');
        },
      });
    const asyncError = () =>
      new Writable({
        write(_chunk, _encoding, cb) {
          cb(new Error('async'));
        },
      });

    // With the default ignoreErrors, write errors are swallowed whether the
    // stream throws synchronously or reports the error through its callback.
    for (const stream of [syncThrow(), asyncError()]) {
      const c = new Console({ stdout: stream, stderr: stream });
      doesNotThrow(() => c.log('a'));
      doesNotThrow(() => c.error('b'));
      // The temporary error listener is removed after each write.
      strictEqual(stream.listenerCount('error'), 0);
    }
    await new Promise((resolve) => setTimeout(resolve, 0));

    // With ignoreErrors: false, a synchronous throw propagates.
    const stream = syncThrow();
    const c = new Console({
      stdout: stream,
      stderr: stream,
      ignoreErrors: false,
    });
    throws(() => c.log('a'), { message: 'sync' });
  },
};

export const testConsoleInspectOptionsMap = {
  async test() {
    const out = capture();
    const err = capture();
    const c = new Console({
      stdout: out.stream,
      stderr: err.stream,
      inspectOptions: new Map([
        [out.stream, { depth: 0 }],
        [err.stream, { compact: false }],
      ]),
    });
    c.log({ a: { b: 1 } });
    strictEqual(out.take(), '{ a: [Object] }\n');
    c.error({ a: 1 });
    strictEqual(err.take(), '{\n  a: 1\n}\n');
  },
};
