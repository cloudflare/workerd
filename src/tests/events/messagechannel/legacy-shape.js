// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Unflagged era: without expose_global_message_channel the classes are not
// globals, and are reached through node:worker_threads (which takes them
// from cloudflare-internal:messagechannel); without set_tostring_tag their
// instances stringify as [object Object].

import { ok, strictEqual } from 'node:assert';
import { MessageChannel, MessagePort } from 'node:worker_threads';
import { usingTsImpl } from 'which-impl';

export const legacyClassesAreNotGlobals = {
  test() {
    ok(!('MessageChannel' in globalThis));
    ok(!('MessagePort' in globalThis));
  },
};

export const legacyClassesThroughWorkerThreads = {
  async test() {
    strictEqual(MessageChannel.name, 'MessageChannel');
    strictEqual(MessagePort.name, 'MessagePort');
    // Divergence #1: only the TypeScript constructor inherits from
    // EventTarget's.
    strictEqual(
      Object.getPrototypeOf(MessagePort) === EventTarget,
      usingTsImpl
    );

    const { port1, port2 } = new MessageChannel();
    ok(port1 instanceof MessagePort);
    ok(port1 instanceof EventTarget);
    const { promise, resolve } = Promise.withResolvers();
    port2.onmessage = resolve;
    port1.postMessage({ hello: 'world' });
    const event = await promise;
    strictEqual(event.data.hello, 'world');
    strictEqual(event.source, port2);
    strictEqual(new MessageEvent('message', { source: port1 }).source, port1);
    port1.close();
  },
};

export const legacyToStringTag = {
  test() {
    const channel = new MessageChannel();
    strictEqual(
      Object.getOwnPropertyDescriptor(
        MessagePort.prototype,
        Symbol.toStringTag
      ),
      undefined
    );
    strictEqual(
      Object.prototype.toString.call(channel.port1),
      '[object Object]'
    );
    strictEqual(Object.prototype.toString.call(channel), '[object Object]');
  },
};
