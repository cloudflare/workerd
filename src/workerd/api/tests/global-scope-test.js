import {
  deepStrictEqual,
  strictEqual,
  throws,
  doesNotThrow,
  notStrictEqual,
  ok,
} from 'node:assert';

import { AsyncLocalStorage } from 'node:async_hooks';
import util from 'node:util';

export const navigatorUserAgent = {
  async test() {
    strictEqual(navigator.userAgent, 'Cloudflare-Workers');
  },
};

export const timeoutClamping = {
  async test() {
    // p2 should resolve, p1 should never resolve.

    const p1 = new Promise((resolve) => {
      setTimeout(() => {
        resolve('b');
      }, 9223372036854775808); // Will be clamped to a max of 3153600000000
    });

    const p2 = new Promise((resolve) => {
      setTimeout(() => {
        resolve('a');
      }, -100); // Will be clamped to 0
    });

    strictEqual(await Promise.race([p1, p2]), 'a');
  },
};

export const timeoutCount = {
  test() {
    for (let i = 0; i < 10000; ++i) {
      setTimeout(() => {});
    }
    throws(() => setTimeout(() => {}), {
      message:
        'You have exceeded the number of active timeouts you may set. max active timeouts: 10000, current active timeouts: 10000, finished timeouts: 0',
    });
  },
};

export const timeoutImplicitCancel = {
  test() {
    // Timeouts should be implicitly canceled after the IoContext is destroyed.
    setTimeout(() => {
      throw new Error('boom');
    }, 100000);
  },
};

export const timeoutVarargs = {
  async test() {
    let resolve;
    const promise = new Promise((a) => (resolve = a));
    setTimeout(
      (a, b, c) => {
        resolve([a, b, c]);
      },
      10,
      1,
      2,
      3
    );
    const results = await promise;
    deepStrictEqual(results, [1, 2, 3]);
  },
};

export const intervalVarargs = {
  async test() {
    let resolve;
    const promise = new Promise((a) => (resolve = a));

    // prettier-ignore
    const i = setInterval((a,b,c) => {
      resolve([a,b,c]);
      clearInterval(i);
    }, 10, 1, 2, 3);

    const results = await promise;
    deepStrictEqual(results, [1, 2, 3]);
  },
};

export const mutableGlobals = {
  async test() {
    {
      const oldCaches = globalThis.caches;
      const oldCrypto = globalThis.crypto;
      const oldSelf = globalThis.self;
      const oldScheduler = globalThis.scheduler;
      delete globalThis.caches;
      delete globalThis.crypto;
      delete globalThis.self;
      delete globalThis.scheduler;
      strictEqual(globalThis.caches, undefined);
      strictEqual(globalThis.crypto, undefined);
      strictEqual(globalThis.self, undefined);
      strictEqual(globalThis.scheduler, undefined);
      Object.defineProperties(globalThis, {
        caches: { value: 'hello there', writable: true, configurable: true },
        crypto: { value: 'we are', writable: true, configurable: true },
        self: { value: 'mutable', writable: true, configurable: true },
        scheduler: {
          value: 'at global scope',
          writable: true,
          configurable: true,
        },
      });
      strictEqual(globalThis.caches, 'hello there');
      strictEqual(globalThis.crypto, 'we are');
      strictEqual(globalThis.self, 'mutable');
      strictEqual(globalThis.scheduler, 'at global scope');
      globalThis.caches = oldCaches;
      globalThis.crypto = oldCrypto;
      globalThis.self = oldSelf;
      globalThis.scheduler = oldScheduler;
    }
  },
};

export const queueMicrotask = {
  async test() {
    [1, undefined, 'hello'].forEach((i) => {
      throws(() => globalThis.queueMicrotask(i), {
        message:
          "Failed to execute 'queueMicrotask' on 'ServiceWorkerGlobalScope': " +
          "parameter 1 is not of type 'Function'.",
      });
    });
    let resolve;
    const promise = new Promise((a) => (resolve = a));
    globalThis.queueMicrotask(resolve);
    await promise;
  },
};

export const unhandledRejectionHandler = {
  async test() {
    let resolve;
    const promise = new Promise((a) => (resolve = a));
    addEventListener(
      'unhandledrejection',
      (event) => {
        resolve();
      },
      { once: true }
    );
    // This should trigger the unhandledrejection handler
    Promise.reject('boom');
    await promise;
  },
};

export const unhandledRejectionHandler2 = {
  async test() {
    let resolve;
    const promise = new Promise((a) => (resolve = a));
    const handler = (event) => {
      throw new Error('should not have fired');
    };
    addEventListener('unhandledrejection', handler);
    try {
      await Promise.reject('boom');
    } catch {}
    await Promise.reject('boom').catch(() => {});
    removeEventListener('unhandledrejection', handler);
  },
};

export const unhandledRejectionHandler3 = {
  async test() {
    let resolve, resolve2;
    const promise = new Promise((a) => (resolve = a));
    const promise2 = new Promise((a) => (resolve2 = a));
    addEventListener('unhandledrejection', (event) => {
      event.promise.catch(() => {});
      resolve();
    });
    addEventListener('rejectionhandled', (event) => {
      resolve2();
    });
    Promise.reject('boom');
    await Promise.all([promise, promise2]);
  },
};

export const unhandledRejectionHandler4 = {
  async test() {
    let resolve;
    const promise = new Promise((a) => (resolve = a));
    addEventListener('unhandledrejection', (event) => {
      throw new Error('does not crash. safe to ignore in test logs');
    });

    Promise.reject('boom');
  },
};

export const structuredClone = {
  test() {
    {
      strictEqual(globalThis.structuredClone('hello'), 'hello');
    }

    {
      const a = {
        b: {
          c: [
            {
              d: 1,
            },
          ],
        },
      };
      const clone = globalThis.structuredClone(a);
      a.b.c[0].d = 2;
      deepStrictEqual(clone.b.c[0].d, 1);
    }

    {
      const enc = new TextEncoder();
      const dec = new TextDecoder();
      const u8 = enc.encode('hello');
      const clone = globalThis.structuredClone(u8);
      strictEqual(dec.decode(clone), 'hello');
    }

    {
      const enc = new TextEncoder();
      const dec = new TextDecoder();
      const u8 = enc.encode('hello');
      const clone = globalThis.structuredClone({ a: u8 });
      u8[0] = 1;
      strictEqual(dec.decode(u8), '\u0001ello');
      strictEqual(dec.decode(clone.a), 'hello');
    }

    {
      const enc = new TextEncoder();
      const dec = new TextDecoder();
      const u8 = enc.encode('hello');
      const clone = globalThis.structuredClone(
        { a: u8 },
        { transfer: [u8.buffer] }
      );
      strictEqual(u8.byteLength, 0);
      strictEqual(dec.decode(clone.a), 'hello');
    }

    {
      const u8 = new Uint8Array(new SharedArrayBuffer(1));
      const clone = globalThis.structuredClone(u8);
      notStrictEqual(clone.buffer, u8.buffer);
      u8[0] = 1;
      strictEqual(clone[0], 1);
    }

    {
      const sab = new SharedArrayBuffer(2);
      const a = {
        b: new Uint8Array(sab),
        c: new Uint16Array(sab),
      };
      const clone = globalThis.structuredClone(a);
      ok(clone.b instanceof Uint8Array);
      ok(clone.c instanceof Uint16Array);

      clone.c[0] = 0xffff;

      strictEqual(a.b[0], 255);
      strictEqual(a.b[1], 255);
    }

    {
      const u8 = new Uint8Array(1);
      globalThis.structuredClone(u8, { transfer: [u8.buffer, u8.buffer] });
    }

    // Non-transferable objects throw
    throws(() => globalThis.structuredClone({}, { transfer: {} }));

    const memory = new WebAssembly.Memory({ initial: 2, maximum: 2 });
    throws(() => globalThis.structuredClone(memory));
  },
};

export const base64 = {
  test() {
    function format_value(elem) {
      return elem;
    }
    // Cloudflare note: The real format_value() is in testharness.js.

    function generate_tests(f, cases) {
      // Cloudflare note: the real generate_tests() is in the real testharness.js, and presumably it
      // calls test(t[0], () => { f(t[1]) }), or something, for each `t` in `cases`. We can forgo the
      // description element (t[0]) for our purposes.
      cases.forEach(([description, testCase]) => {
        f(testCase);
      });
    }

    /**
     * btoa() as defined by the HTML5 spec, which mostly just references RFC4648.
     */
    function mybtoa(s) {
      // String conversion as required by WebIDL.
      s = String(s);

      // "The btoa() method must throw an INVALID_CHARACTER_ERR exception if the
      // method's first argument contains any character whose code point is
      // greater than U+00FF."
      for (var i = 0; i < s.length; i++) {
        if (s.charCodeAt(i) > 255) {
          return 'INVALID_CHARACTER_ERR';
        }
      }

      var out = '';
      for (var i = 0; i < s.length; i += 3) {
        var groupsOfSix = [undefined, undefined, undefined, undefined];
        groupsOfSix[0] = s.charCodeAt(i) >> 2;
        groupsOfSix[1] = (s.charCodeAt(i) & 0x03) << 4;
        if (s.length > i + 1) {
          groupsOfSix[1] |= s.charCodeAt(i + 1) >> 4;
          groupsOfSix[2] = (s.charCodeAt(i + 1) & 0x0f) << 2;
        }
        if (s.length > i + 2) {
          groupsOfSix[2] |= s.charCodeAt(i + 2) >> 6;
          groupsOfSix[3] = s.charCodeAt(i + 2) & 0x3f;
        }
        for (var j = 0; j < groupsOfSix.length; j++) {
          if (typeof groupsOfSix[j] == 'undefined') {
            out += '=';
          } else {
            out += btoaLookup(groupsOfSix[j]);
          }
        }
      }
      return out;
    }

    /**
     * Lookup table for mybtoa(), which converts a six-bit number into the
     * corresponding ASCII character.
     */
    function btoaLookup(idx) {
      if (idx < 26) {
        return String.fromCharCode(idx + 'A'.charCodeAt(0));
      }
      if (idx < 52) {
        return String.fromCharCode(idx - 26 + 'a'.charCodeAt(0));
      }
      if (idx < 62) {
        return String.fromCharCode(idx - 52 + '0'.charCodeAt(0));
      }
      if (idx == 62) {
        return '+';
      }
      if (idx == 63) {
        return '/';
      }
      // Throw INVALID_CHARACTER_ERR exception here -- won't be hit in the tests.
    }

    /**
     * Implementation of atob() according to the HTML spec, except that instead of
     * throwing INVALID_CHARACTER_ERR we return null.
     */
    function myatob(input) {
      // WebIDL requires DOMStrings to just be converted using ECMAScript
      // ToString, which in our case amounts to calling String().
      input = String(input);

      // "Remove all space characters from input."
      input = input.replace(/[ \t\n\f\r]/g, '');

      // "If the length of input divides by 4 leaving no remainder, then: if
      // input ends with one or two U+003D EQUALS SIGN (=) characters, remove
      // them from input."
      if (input.length % 4 == 0 && /==?$/.test(input)) {
        input = input.replace(/==?$/, '');
      }

      // "If the length of input divides by 4 leaving a remainder of 1, throw an
      // INVALID_CHARACTER_ERR exception and abort these steps."
      //
      // "If input contains a character that is not in the following list of
      // characters and character ranges, throw an INVALID_CHARACTER_ERR
      // exception and abort these steps:
      //
      // U+002B PLUS SIGN (+)
      // U+002F SOLIDUS (/)
      // U+0030 DIGIT ZERO (0) to U+0039 DIGIT NINE (9)
      // U+0041 LATIN CAPITAL LETTER A to U+005A LATIN CAPITAL LETTER Z
      // U+0061 LATIN SMALL LETTER A to U+007A LATIN SMALL LETTER Z"
      if (input.length % 4 == 1 || !/^[+/0-9A-Za-z]*$/.test(input)) {
        return null;
      }

      // "Let output be a string, initially empty."
      var output = '';

      // "Let buffer be a buffer that can have bits appended to it, initially
      // empty."
      //
      // We append bits via left-shift and or.  accumulatedBits is used to track
      // when we've gotten to 24 bits.
      var buffer = 0;
      var accumulatedBits = 0;

      // "While position does not point past the end of input, run these
      // substeps:"
      for (var i = 0; i < input.length; i++) {
        // "Find the character pointed to by position in the first column of
        // the following table. Let n be the number given in the second cell of
        // the same row."
        //
        // "Append to buffer the six bits corresponding to number, most
        // significant bit first."
        //
        // atobLookup() implements the table from the spec.
        buffer <<= 6;
        buffer |= atobLookup(input[i]);

        // "If buffer has accumulated 24 bits, interpret them as three 8-bit
        // big-endian numbers. Append the three characters with code points
        // equal to those numbers to output, in the same order, and then empty
        // buffer."
        accumulatedBits += 6;
        if (accumulatedBits == 24) {
          output += String.fromCharCode((buffer & 0xff0000) >> 16);
          output += String.fromCharCode((buffer & 0xff00) >> 8);
          output += String.fromCharCode(buffer & 0xff);
          buffer = accumulatedBits = 0;
        }

        // "Advance position by one character."
      }

      // "If buffer is not empty, it contains either 12 or 18 bits. If it
      // contains 12 bits, discard the last four and interpret the remaining
      // eight as an 8-bit big-endian number. If it contains 18 bits, discard the
      // last two and interpret the remaining 16 as two 8-bit big-endian numbers.
      // Append the one or two characters with code points equal to those one or
      // two numbers to output, in the same order."
      if (accumulatedBits == 12) {
        buffer >>= 4;
        output += String.fromCharCode(buffer);
      } else if (accumulatedBits == 18) {
        buffer >>= 2;
        output += String.fromCharCode((buffer & 0xff00) >> 8);
        output += String.fromCharCode(buffer & 0xff);
      }

      // "Return output."
      return output;
    }

    /**
     * A lookup table for atob(), which converts an ASCII character to the
     * corresponding six-bit number.
     */
    function atobLookup(chr) {
      if (/[A-Z]/.test(chr)) {
        return chr.charCodeAt(0) - 'A'.charCodeAt(0);
      }
      if (/[a-z]/.test(chr)) {
        return chr.charCodeAt(0) - 'a'.charCodeAt(0) + 26;
      }
      if (/[0-9]/.test(chr)) {
        return chr.charCodeAt(0) - '0'.charCodeAt(0) + 52;
      }
      if (chr == '+') {
        return 62;
      }
      if (chr == '/') {
        return 63;
      }
      // Throw exception; should not be hit in tests
    }

    function testBtoa(input) {
      // "The btoa() method must throw an INVALID_CHARACTER_ERR exception if the
      // method's first argument contains any character whose code point is
      // greater than U+00FF."
      var normalizedInput = String(input);
      for (var i = 0; i < normalizedInput.length; i++) {
        if (normalizedInput.charCodeAt(i) > 255) {
          throws(() => globalThis.btoa(input));
          return;
        }
      }
      strictEqual(globalThis.btoa(input), mybtoa(input));
      strictEqual(globalThis.atob(globalThis.btoa(input)), String(input));
    }

    var tests = [
      'עברית',
      '',
      'ab',
      'abc',
      'abcd',
      'abcde',
      // This one is thrown in because IE9 seems to fail atob(btoa()) on it.  Or
      // possibly to fail btoa().  I actually can't tell what's happening here,
      // but it doesn't hurt.
      '\xff\xff\xc0',
      // Is your DOM implementation binary-safe?
      '\0a',
      'a\0b',
      // WebIDL tests.
      undefined,
      null,
      7,
      12,
      1.5,
      true,
      false,
      NaN,
      +Infinity,
      -Infinity,
      0,
      -0,
      {
        toString: function () {
          return 'foo';
        },
      },
    ];
    for (var i = 0; i < 258; i++) {
      tests.push(String.fromCharCode(i));
    }
    tests.push(String.fromCharCode(10000));
    tests.push(String.fromCharCode(65534));
    tests.push(String.fromCharCode(65535));

    // This is supposed to be U+10000.
    tests.push(String.fromCharCode(0xd800, 0xdc00));
    tests = tests.map(function (elem) {
      var expected = mybtoa(elem);
      if (expected === 'INVALID_CHARACTER_ERR') {
        return [
          'btoa(' + format_value(elem) + ') must raise INVALID_CHARACTER_ERR',
          elem,
        ];
      }
      return [
        'btoa(' + format_value(elem) + ') == ' + format_value(mybtoa(elem)),
        elem,
      ];
    });

    var everything = '';
    for (var i = 0; i < 256; i++) {
      everything += String.fromCharCode(i);
    }
    tests.push(['btoa(first 256 code points concatenated)', everything]);

    generate_tests(testBtoa, tests);

    function testAtob(input) {
      var expected = myatob(input);
      if (expected === null) {
        throws(() => atob(input));
        return;
      }

      strictEqual(globalThis.atob(input), expected);
    }

    var tests = [
      '',
      'abcd',
      ' abcd',
      'abcd ',
      ' abcd===',
      'abcd=== ',
      'abcd ===',
      'a',
      'ab',
      'abc',
      'abcde',
      String.fromCharCode(0xd800, 0xdc00),
      '=',
      '==',
      '===',
      '====',
      '=====',
      'a=',
      'a==',
      'a===',
      'a====',
      'a=====',
      'ab=',
      'ab==',
      'ab===',
      'ab====',
      'ab=====',
      'abc=',
      'abc==',
      'abc===',
      'abc====',
      'abc=====',
      'abcd=',
      'abcd==',
      'abcd===',
      'abcd====',
      'abcd=====',
      'abcde=',
      'abcde==',
      'abcde===',
      'abcde====',
      'abcde=====',
      '=a',
      '=a=',
      'a=b',
      'a=b=',
      'ab=c',
      'ab=c=',
      'abc=d',
      'abc=d=',
      // With whitespace
      'ab\tcd',
      'ab\ncd',
      'ab\fcd',
      'ab\rcd',
      'ab cd',
      'ab\u00a0cd',
      'ab\t\n\f\r cd',
      ' \t\n\f\r ab\t\n\f\r cd\t\n\f\r ',
      'ab\t\n\f\r =\t\n\f\r =\t\n\f\r ',
      // Test if any bits are set at the end.  These should all be fine, since
      // they end with A, which becomes 0:
      'A',
      '/A',
      '//A',
      '///A',
      '////A',
      // These are all bad, since they end in / (= 63, all bits set) but their
      // length isn't a multiple of four characters, so they can't be output by
      // btoa().  Thus one might expect some UAs to throw exceptions or otherwise
      // object, since they could never be output by btoa(), so they're good to
      // test.
      '/',
      'A/',
      'AA/',
      'AAAA/',
      // But this one is possible:
      'AAA/',
      // Binary-safety tests
      '\0nonsense',
      'abcd\0nonsense',
      // WebIDL tests
      // TODO(conform): We need automatic stringification of arguments before these will work.
      //undefined, null, 7, 12, 1.5, true, false, NaN, +Infinity, -Infinity, 0, -0,
      //{ toString: function () { return "foo" } },
      //{ toString: function () { return "abcd" } },
    ];
    tests = tests.map(function (elem) {
      if (myatob(elem) === null) {
        return [
          'atob(' + format_value(elem) + ') must raise InvalidCharacterError',
          elem,
        ];
      }
      return [
        'atob(' + format_value(elem) + ') == ' + format_value(myatob(elem)),
        elem,
      ];
    });

    generate_tests(testAtob, tests);
  },
};

export const webSocketPairIterable = {
  test() {
    const [a, b] = new WebSocketPair();
    ok(a instanceof WebSocket);
    ok(b instanceof WebSocket);
  },
};

export const queueMicrotaskError = {
  async test() {
    const als = new AsyncLocalStorage();
    const { promise, resolve } = Promise.withResolvers();
    const err = new Error('boom');
    err.resolve = resolve;

    addEventListener(
      'error',
      (event) => {
        // Verify that async context propagates correctly
        // in the error event.
        strictEqual(als.getStore(), 123);
        // We got the correct expected error.
        strictEqual(event.error, err);
        event.error.resolve();
        // Returning true suppresses the default logging.
        return true;
      },
      { once: true }
    );

    als.run(123, () =>
      globalThis.queueMicrotask(() => {
        // Throwing inside a queueMicrotask should trigger the error event.
        strictEqual(als.getStore(), 123);
        throw err;
      })
    );

    await promise;
  },
};

export const toStringTag = {
  test() {
    strictEqual('Response', Response.prototype[Symbol.toStringTag]);
    strictEqual('Request', Request.prototype[Symbol.toStringTag]);
    strictEqual('Headers', Headers.prototype[Symbol.toStringTag]);
    strictEqual(
      'URLSearchParams',
      URLSearchParams.prototype[Symbol.toStringTag]
    );
    strictEqual('CryptoKey', CryptoKey.prototype[Symbol.toStringTag]);
    strictEqual(
      toString.call(new URLSearchParams()),
      '[object URLSearchParams]'
    );

    const internalFlag = Symbol.for('cloudflare:internal-class');
    strictEqual(Headers.prototype[internalFlag], internalFlag);
    strictEqual(new Headers()[internalFlag], internalFlag);
  },
};
export const validateGlobalThis = {
  test() {
    util.inspect(globalThis);
  },
};
