// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A DOMException is serializable. The C++ class writes its name, message
// and stack as tag 8, the stack converted to a string. V8's writes them as
// tag 24, the stack as whatever value its own `stack` holds, and, with
// enhanced error serialization, its other own properties too, as other
// errors do (dom-exception-bootstrap-test.c++ pins the bytes). Tags 7, 8
// and 24 read back as instances of the global DOMException.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { env } from 'cloudflare:workers';
import { enhancedErrorSerialization, usingV8Impl } from 'which-impl';
import { assertDomException, attributesOf } from 'helpers';

// Deserialized stacks are own data properties, enumerable only in the C++
// (ledger #2).
const kClonedStackAttributes = usingV8Impl ? 'datawc' : 'datawe';

export const structuredCloneRoundTrip = {
  test() {
    const error = new DOMException('m', 'AbortError');
    const clone = structuredClone(error);
    assertDomException(clone, { name: 'AbortError', message: 'm', code: 20 });
    strictEqual(clone.stack, error.stack);
    strictEqual(attributesOf(clone, 'stack'), kClonedStackAttributes);
  },
};

export const nativeInstancesKeepTheirStack = {
  test() {
    let error;
    try {
      atob('!');
    } catch (e) {
      error = e;
    }
    strictEqual(structuredClone(error).stack, error.stack);
    const reason = AbortSignal.abort().reason;
    strictEqual(structuredClone(reason).stack, reason.stack);
  },
};

export const subclassesCloneAsDOMException = {
  test() {
    class Sub extends DOMException {}
    const clone = structuredClone(new Sub('m', 'NotFoundError'));
    strictEqual(Object.getPrototypeOf(clone), DOMException.prototype);
    assertDomException(clone, { name: 'NotFoundError', message: 'm', code: 8 });
  },
};

// What the attributes of V8's `error` return, past any own properties that
// shadow them.
function attributeValues(error) {
  return ['name', 'message', 'code'].map((attribute) =>
    Object.getOwnPropertyDescriptor(DOMException.prototype, attribute).get.call(
      error
    )
  );
}

export const otherOwnPropertiesAreCloned = {
  test() {
    const error = new DOMException('m', 'AbortError');
    error.extra = 1;
    Object.defineProperty(error, 'name', { value: 'Other' });
    Object.defineProperty(error, 'message', { value: 'other' });
    Object.defineProperty(error, 'hidden', { value: { x: 2 } });
    error[Symbol('s')] = 3;
    error[0] = 4;
    error.stack = 'S';
    const clone = structuredClone(error);
    if (!usingV8Impl || !enhancedErrorSerialization) {
      // Only its name, message and stack.
      assertDomException(clone, {
        name: 'AbortError',
        message: 'm',
        code: 20,
        stack: 'S',
      });
      return;
    }
    // Its other own properties too, those that shadow its attributes
    // included, each an enumerable data property. As for other errors, those
    // keyed by a symbol or an index are not cloned.
    deepStrictEqual(attributeValues(clone), ['AbortError', 'm', 20]);
    deepStrictEqual(Reflect.ownKeys(clone), [
      'stack',
      'extra',
      'name',
      'message',
      'hidden',
    ]);
    deepStrictEqual(
      [clone.stack, clone.extra, clone.name, clone.message, clone.hidden],
      ['S', 1, 'Other', 'other', { x: 2 }]
    );
    strictEqual(attributesOf(clone, 'hidden'), 'datawec');
  },
};

export const stackValuesAreCloned = {
  test() {
    const cloneWithStack = (stack) => {
      const error = new DOMException('m', 'AbortError');
      error.stack = stack;
      return structuredClone(error);
    };
    const withoutStack = new DOMException('m', 'AbortError');
    delete withoutStack.stack;
    if (!usingV8Impl) {
      // The C++ class converts its stack to a string, which is empty if the
      // stack is undefined or it has none.
      strictEqual(cloneWithStack(null).stack, 'null');
      strictEqual(cloneWithStack(undefined).stack, '');
      strictEqual(structuredClone(withoutStack).stack, '');
      return;
    }
    // V8's clones its stack as whatever value it holds, and has none if it
    // had none.
    for (const stack of [undefined, null, 5, { frames: ['f'] }, ['f']]) {
      const clone = cloneWithStack(stack);
      deepStrictEqual(clone.stack, stack);
      strictEqual(attributesOf(clone, 'stack'), 'datawc');
    }
    strictEqual(Object.hasOwn(structuredClone(withoutStack), 'stack'), false);
    const accessor = new DOMException('m', 'AbortError');
    Object.defineProperty(accessor, 'stack', { get: () => 'got' });
    strictEqual(structuredClone(accessor).stack, 'got');
  },
};

// V8 lets nothing refer to a host object until it has been read, so a
// DOMException can't be read if its stack, or any of its other own
// properties that are written, refers back to it.
export const selfReferencesCannotBeRead = {
  test() {
    if (!usingV8Impl) return;
    const cannotBeRead = (error) => {
      throws(() => structuredClone(error), {
        name: 'Error',
        message: 'Unable to deserialize cloned data.',
      });
    };
    const references = [
      (e) => e,
      (e) => ({ b: e }),
      (e) => [[e]],
      (e) => new Map([[1, { e }]]),
      (e) => new Set([e]),
    ];
    for (const reference of references) {
      const withStack = new DOMException('m', 'AbortError');
      withStack.stack = reference(withStack);
      cannotBeRead(withStack);

      const withProperty = new DOMException('m', 'AbortError');
      withProperty.a = reference(withProperty);
      if (enhancedErrorSerialization) {
        cannotBeRead(withProperty);
      } else {
        strictEqual(structuredClone(withProperty).a, undefined);
      }
    }
    // A reference from outside it is read.
    const error = new DOMException('m', 'AbortError');
    const clone = structuredClone({ error, again: { error } });
    strictEqual(clone.again.error, clone.error);
  },
};

export const nestedAndPosted = {
  async test() {
    const error = new DOMException('m', 'AbortError');
    const clone = structuredClone({ errors: [error, error] });
    strictEqual(clone.errors[0], clone.errors[1]);
    assertDomException(clone.errors[0], {
      name: 'AbortError',
      message: 'm',
      code: 20,
    });

    const { port1, port2 } = new MessageChannel();
    try {
      const received = new Promise((resolve) => {
        port2.onmessage = ({ data }) => resolve(data);
      });
      port1.postMessage(error);
      assertDomException(await received, {
        name: 'AbortError',
        message: 'm',
        code: 20,
      });
    } finally {
      port1.close();
      port2.close();
    }
  },
};

// The bodies of the messages main.js's queue() handler receives, by id.
export const queued = new Map();

function varint(n) {
  const bytes = [];
  do {
    let b = n & 0x7f;
    n >>>= 7;
    if (n > 0) b |= 0x80;
    bytes.push(b);
  } while (n > 0);
  return bytes;
}

function lengthDelimited(s) {
  const bytes = [...new TextEncoder().encode(s)];
  return [...varint(bytes.length), ...bytes];
}

function v8String(s) {
  const bytes = Array.from(s, (c) => c.charCodeAt(0));
  return [0x22, ...varint(bytes.length), ...bytes];
}

function v8Int32(n) {
  return [0x49, ...varint((n << 1) ^ (n >> 31))];
}

const kV8Null = 0x30;

const kV8Header = [0xff, 0x0f];
const kHostObject = 0x5c;

async function deliver(id, bytes) {
  const result = await env.SELF.queue('dom-exception', [
    {
      id,
      timestamp: new Date(),
      serializedBody: new Uint8Array(bytes).buffer,
      attempts: 1,
    },
  ]);
  strictEqual(result.outcome, 'ok');
  return queued.get(id);
}

export const readsTag8 = {
  async test() {
    const body = await deliver('tag-8', [
      ...kV8Header,
      kHostObject,
      8,
      ...lengthDelimited('NotFoundError'),
      ...lengthDelimited('m'),
      ...lengthDelimited('S'),
    ]);
    assertDomException(body, {
      name: 'NotFoundError',
      message: 'm',
      code: 8,
      stack: 'S',
    });
  },
};

export const readsTag7 = {
  async test() {
    const body = await deliver('tag-7', [
      ...kV8Header,
      kHostObject,
      7,
      ...lengthDelimited('NotFoundError'),
      0x6f,
      ...v8String('message'),
      ...v8String('m'),
      ...v8String('stack'),
      ...v8String('S'),
      0x7b,
      2,
    ]);
    assertDomException(body, {
      name: 'NotFoundError',
      message: 'm',
      code: 8,
      stack: 'S',
    });
  },
};

export const readsUtf8 = {
  async test() {
    const body = await deliver('tag-8-utf8', [
      ...kV8Header,
      kHostObject,
      8,
      ...lengthDelimited('Ünïcode'),
      ...lengthDelimited('m\u{1F600}'),
      ...lengthDelimited(''),
    ]);
    deepStrictEqual(
      [body.name, body.message, body.code],
      ['Ünïcode', 'm\u{1F600}', 0]
    );
  },
};

// V8's DOMException as tag 24 writes it: its name and message, whether it
// has an own `stack` and the value, and its other own properties.
function tag24(name, message, stack, properties) {
  return [
    ...kV8Header,
    kHostObject,
    24,
    ...lengthDelimited(name),
    ...lengthDelimited(message),
    ...stack,
    ...varint(properties.length),
    ...properties.flat(),
  ];
}

export const readsTag24 = {
  async test() {
    const body = await deliver(
      'tag-24',
      tag24(
        'NotFoundError',
        'm',
        [1, ...v8String('S')],
        [[...v8String('extra'), ...v8Int32(1)]]
      )
    );
    // The C++ class drops the other own properties.
    assertDomException(
      body,
      { name: 'NotFoundError', message: 'm', code: 8, stack: 'S' },
      ...(usingV8Impl ? ['extra'] : [])
    );
    strictEqual(body.extra, usingV8Impl ? 1 : undefined);

    const nullStack = await deliver(
      'tag-24-null-stack',
      tag24('NotFoundError', 'm', [1, kV8Null], [])
    );
    strictEqual(nullStack.stack, null);

    // Without a stack, V8's has none, and the C++ class's is undefined.
    const noStack = await deliver(
      'tag-24-no-stack',
      tag24('NotFoundError', 'm', [0], [])
    );
    strictEqual(Object.hasOwn(noStack, 'stack'), !usingV8Impl);
    strictEqual(noStack.stack, undefined);
  },
};
