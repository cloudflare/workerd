// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// navigator.sendBeacon(): its shape, receiver and arguments, and the POST
// it schedules. Beacons go to the sink service (sink.js), the workers'
// global outbound.

import { deepStrictEqual, ok, strictEqual } from 'node:assert';
import { specCompliantPropertyAttributes, usingTsImpl } from 'which-impl';
import {
  caught,
  framesOf,
  isBootstrapFrame,
  kIllegalInvocation,
  kMissingUrl,
  kSymbolArgument,
  received,
  throwsTypeError,
} from 'helpers';
import { topLevel } from 'top-level';

const { sendBeacon } = Navigator.prototype;

export const methodShape = {
  test() {
    strictEqual(typeof sendBeacon, 'function');
    strictEqual(navigator.sendBeacon, sendBeacon);
    strictEqual(Object.hasOwn(navigator, 'sendBeacon'), false);
    deepStrictEqual(Reflect.ownKeys(sendBeacon), ['length', 'name']);
    strictEqual(sendBeacon.name, 'sendBeacon');
    // JSG counts the required argument only under
    // spec_compliant_property_attributes.
    strictEqual(sendBeacon.length, specCompliantPropertyAttributes ? 1 : 0);
    strictEqual(Object.getPrototypeOf(sendBeacon), Function.prototype);
    // Not a constructor.
    strictEqual('prototype' in sendBeacon, false);
    ok(caught(() => new sendBeacon('http://sink/')) instanceof TypeError);
    ok(
      caught(() => Reflect.construct(Object, [], sendBeacon)) instanceof
        TypeError
    );
  },
};

export const receiver = {
  test() {
    const receivers = [
      undefined,
      null,
      {},
      globalThis,
      Navigator,
      Navigator.prototype,
      // Inheriting from the navigator, or from its prototype, makes no
      // Navigator.
      Object.create(navigator),
      Object.create(Navigator.prototype),
      new Proxy(navigator, {}),
      'navigator',
      1,
    ];
    for (const value of receivers) {
      throwsTypeError(
        () => sendBeacon.call(value, 'http://sink/receiver'),
        kIllegalInvocation
      );
    }
  },
};

export const receiverIsCheckedFirst = {
  test() {
    // Before the argument count, and before any conversion.
    throwsTypeError(() => sendBeacon.call({}), kIllegalInvocation);
    let converted = false;
    const url = {
      toString() {
        converted = true;
        return 'http://sink/receiver-first';
      },
    };
    throwsTypeError(() => sendBeacon.call({}, url), kIllegalInvocation);
    throwsTypeError(() => sendBeacon.call({}, Symbol()), kIllegalInvocation);
    strictEqual(converted, false);
  },
};

export const missingArgument = {
  test() {
    throwsTypeError(() => navigator.sendBeacon(), kMissingUrl);
  },
};

export const returnsTrueWithinARequest = {
  async test() {
    strictEqual(navigator.sendBeacon('http://sink/returns-true'), true);
    deepStrictEqual(await received('/returns-true'), {
      method: 'POST',
      contentType: null,
      body: '',
      size: 0,
    });
  },
};

export const returnsFalseOutsideARequest = {
  test() {
    strictEqual(topLevel.plain, false);
    strictEqual(topLevel.withBody, false);
    strictEqual(topLevel.invalidUrl, false);
    // The arguments are converted all the same, the URL first.
    strictEqual(topLevel.converted, false);
    deepStrictEqual(topLevel.log, ['url', 'body']);
    // And the errors are the same.
    strictEqual(topLevel.missingArgument.message, kMissingUrl);
    strictEqual(topLevel.symbolUrl.message, kSymbolArgument);
    strictEqual(topLevel.symbolBody.message, kSymbolArgument);
    strictEqual(topLevel.receiver.message, kIllegalInvocation);
    for (const name of [
      'missingArgument',
      'symbolUrl',
      'symbolBody',
      'receiver',
    ]) {
      ok(topLevel[name] instanceof TypeError, name);
    }
  },
};

export const urlIsConvertedToAString = {
  async test() {
    // ToString, so undefined and null are their names, which are not URLs:
    // the beacon is scheduled and its fetch fails.
    strictEqual(
      navigator.sendBeacon({ toString: () => 'http://sink/url-object' }),
      true
    );
    await received('/url-object');
    strictEqual(
      navigator.sendBeacon({
        [Symbol.toPrimitive]: (hint) => `http://sink/url-${hint}`,
      }),
      true
    );
    await received('/url-string');
    strictEqual(navigator.sendBeacon(new URL('http://sink/url-url')), true);
    await received('/url-url');
    strictEqual(
      navigator.sendBeacon(new String('http://sink/url-boxed')),
      true
    );
    await received('/url-boxed');
  },
};

export const symbols = {
  test() {
    throwsTypeError(() => navigator.sendBeacon(Symbol('url')), kSymbolArgument);
    throwsTypeError(
      () => navigator.sendBeacon('http://sink/symbol', Symbol('body')),
      kSymbolArgument
    );
    throwsTypeError(
      () => navigator.sendBeacon(Symbol.iterator, Symbol('body')),
      kSymbolArgument
    );
  },
};

export const conversionOrder = {
  async test() {
    const log = [];
    const logs = (name, result) => ({
      toString() {
        log.push(name);
        return result;
      },
    });
    strictEqual(
      navigator.sendBeacon(
        logs('url', 'http://sink/conversion-order'),
        logs('body', 'converted')
      ),
      true
    );
    deepStrictEqual(log, ['url', 'body']);
    strictEqual((await received('/conversion-order')).body, 'converted');

    // A URL that cannot be converted stops before the body is.
    log.length = 0;
    throwsTypeError(
      () => navigator.sendBeacon(Symbol('url'), logs('body', 'unused')),
      kSymbolArgument
    );
    deepStrictEqual(log, []);
  },
};

export const conversionErrorsPropagate = {
  test() {
    const error = new RangeError('thrown by the conversion');
    const throwing = {
      toString() {
        throw error;
      },
    };
    strictEqual(
      caught(() => navigator.sendBeacon(throwing)),
      error
    );
    strictEqual(
      caught(() => navigator.sendBeacon('http://sink/throws', throwing)),
      error
    );
    // Neither conversion gives a string.
    const unconvertible = Object.create(null);
    ok(caught(() => navigator.sendBeacon(unconvertible)) instanceof TypeError);
    ok(
      caught(() =>
        navigator.sendBeacon('http://sink/throws', unconvertible)
      ) instanceof TypeError
    );
  },
};

export const extraArgumentsAreIgnored = {
  async test() {
    let read = false;
    const extra = {
      toString() {
        read = true;
        return '';
      },
    };
    strictEqual(
      navigator.sendBeacon('http://sink/extra-arguments', 'body', extra, extra),
      true
    );
    strictEqual(read, false);
    strictEqual((await received('/extra-arguments')).body, 'body');
  },
};

export const bodies = {
  async test() {
    const encoder = new TextEncoder();
    const form = new FormData();
    form.append('field', 'value');
    const cases = [
      ['undefined', undefined, '', null],
      ['string', 'text', 'text', 'text/plain;charset=UTF-8'],
      ['empty-string', '', '', 'text/plain;charset=UTF-8'],
      // Anything that is no body type is converted to a string.
      ['null', null, 'null', 'text/plain;charset=UTF-8'],
      ['number', 5, '5', 'text/plain;charset=UTF-8'],
      ['boolean', false, 'false', 'text/plain;charset=UTF-8'],
      ['object', {}, '[object Object]', 'text/plain;charset=UTF-8'],
      [
        'to-string',
        { toString: () => 'converted' },
        'converted',
        'text/plain;charset=UTF-8',
      ],
      ['array-buffer', encoder.encode('buffer').buffer, 'buffer', null],
      ['typed-array', encoder.encode('view'), 'view', null],
      [
        'blob',
        new Blob(['blob'], { type: 'application/x-test' }),
        'blob',
        'application/x-test',
      ],
      [
        'search-params',
        new URLSearchParams({ a: '1', b: '2' }),
        'a=1&b=2',
        'application/x-www-form-urlencoded;charset=UTF-8',
      ],
      ['stream', new Blob(['stream']).stream(), 'stream', null],
    ];
    for (const [name, body] of cases) {
      strictEqual(navigator.sendBeacon(`http://sink/body-${name}`, body), true);
    }
    for (const [name, , text, contentType] of cases) {
      const beacon = await received(`/body-${name}`);
      strictEqual(beacon.method, 'POST', name);
      strictEqual(beacon.body, text, name);
      strictEqual(beacon.contentType, contentType, name);
    }

    strictEqual(navigator.sendBeacon('http://sink/body-form-data', form), true);
    const beacon = await received('/body-form-data');
    ok(beacon.contentType.startsWith('multipart/form-data; boundary='));
    ok(beacon.body.includes('name="field"'));
    ok(beacon.body.includes('value'));
  },
};

export const errorStacksStartAtTheCaller = {
  test() {
    function calls(fn) {
      return caught(fn);
    }
    const errors = [
      calls(() => sendBeacon.call({}, 'http://sink/')),
      calls(() => navigator.sendBeacon()),
      calls(() => navigator.sendBeacon(Symbol('url'))),
      calls(() => navigator.sendBeacon('http://sink/', Symbol('body'))),
    ];
    for (const error of errors) {
      const frames = framesOf(error);
      ok(!frames.some(isBootstrapFrame), error.stack);
      // The function making the call, caught(), then calls().
      ok(frames[1].includes('caught'), error.stack);
      ok(frames[2].includes('calls'), error.stack);
    }
    // A new error each time.
    ok(
      calls(() => navigator.sendBeacon()) !==
        calls(() => navigator.sendBeacon())
    );
  },
};

export const stackFramesDuringConversion = {
  test() {
    // Ledger: code that a conversion runs is called by the native method,
    // or through a frame of the TypeScript one.
    const stacks = [];
    const records = {
      toString() {
        stacks.push(new Error('conversion'));
        return 'http://sink/conversion-frames';
      },
    };
    (function caller() {
      navigator.sendBeacon(records, records);
    })();
    strictEqual(stacks.length, 2);
    for (const error of stacks) {
      const frames = framesOf(error);
      ok(frames[0].includes('toString'), error.stack);
      const between = frames.slice(
        1,
        frames.findIndex((frame) => frame.includes('caller'))
      );
      if (usingTsImpl) {
        strictEqual(between.length, 1, error.stack);
        ok(isBootstrapFrame(between[0]), error.stack);
        ok(between[0].includes('sendBeacon'), error.stack);
      } else {
        // The native method has no frame of its own in the stack.
        strictEqual(between.length, 0, error.stack);
      }
    }
  },
};
