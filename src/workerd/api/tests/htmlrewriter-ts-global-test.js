// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The HTMLRewriter global of the TypeScript HTMLRewriter, whose module is loaded on the first
// read of the global. Each service is its own isolate, so each test sees the global unread.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { env } from 'cloudflare:workers';

const prototype = Object.getPrototypeOf(globalThis);
// Under spec_compliant_property_attributes, the global object has its own HTMLRewriter beside
// the prototype's, which is what a read reaches.
const holder = Object.hasOwn(globalThis, 'HTMLRewriter')
  ? globalThis
  : prototype;
const enumerable = holder === prototype;

function attributes(descriptor) {
  return {
    hasGetter: typeof descriptor.get === 'function',
    writable: descriptor.writable,
    enumerable: descriptor.enumerable,
    configurable: descriptor.configurable,
  };
}

export const loadsOnFirstRead = {
  async test() {
    if (env.TEST !== 'read') return;
    deepStrictEqual(
      attributes(Object.getOwnPropertyDescriptor(holder, 'HTMLRewriter')),
      { hasGetter: true, writable: undefined, enumerable, configurable: true }
    );

    // The rewriter's captures of the streams' methods were taken before user code ran.
    const writerPrototype = WritableStreamDefaultWriter.prototype;
    const { write, close } = writerPrototype;
    writerPrototype.write = writerPrototype.close = () => {
      throw new Error('polluted');
    };
    let out;
    try {
      out = await new HTMLRewriter()
        .on('p', {
          element(el) {
            el.setAttribute('a', 'b');
          },
        })
        .transform(new Response('<p>x</p>'))
        .text();
    } finally {
      writerPrototype.write = write;
      writerPrototype.close = close;
    }
    strictEqual(out, '<p a="b">x</p>');

    const descriptor = Object.getOwnPropertyDescriptor(holder, 'HTMLRewriter');
    deepStrictEqual(attributes(descriptor), {
      hasGetter: false,
      writable: true,
      enumerable,
      configurable: true,
    });
    strictEqual(descriptor.value, HTMLRewriter);
    strictEqual(HTMLRewriter.name, 'HTMLRewriter');
  },
};

export const assignmentBeforeFirstRead = {
  test() {
    if (env.TEST !== 'assign') return;
    globalThis.HTMLRewriter = 1;
    strictEqual(HTMLRewriter, 1);
    deepStrictEqual(
      attributes(Object.getOwnPropertyDescriptor(globalThis, 'HTMLRewriter')),
      // An assignment through the prototype makes an own property, enumerable, as it would for
      // the data property of a nested type.
      { hasGetter: false, writable: true, enumerable, configurable: true }
    );
    if (holder === prototype) {
      strictEqual(
        typeof Object.getOwnPropertyDescriptor(prototype, 'HTMLRewriter').get,
        'function'
      );
    }
  },
};
