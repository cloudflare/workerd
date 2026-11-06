// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A DOMException is serializable: its name, message and stack, as tag 8
// (dom-exception-bootstrap-test.c++ pins the bytes). Tags 7 and 8 read
// back as instances of the global DOMException.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { env } from 'cloudflare:workers';
import { usingTsImpl } from 'which-impl';
import { assertDomException, attributesOf } from 'helpers';

// Deserialized stacks are own data properties, enumerable only in the C++
// (ledger #2).
const kClonedStackAttributes = usingTsImpl ? 'datawc' : 'datawe';

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

export const onlyNameMessageAndStackAreCloned = {
  test() {
    const error = new DOMException('m', 'AbortError');
    error.extra = 1;
    Object.defineProperty(error, 'name', { value: 'Other' });
    Object.defineProperty(error, 'message', { value: 'other' });
    error.stack = 'S';
    const clone = structuredClone(error);
    assertDomException(clone, {
      name: 'AbortError',
      message: 'm',
      code: 20,
      stack: 'S',
    });
    strictEqual(clone.extra, undefined);
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
