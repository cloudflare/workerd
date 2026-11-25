// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// node:events emits a MaxListenersExceededWarning through process.emitWarning(), which emits on the
// process object from a microtask. Nothing here loads the process module, so the warning comes
// before the process object exists.
import { EventEmitter } from 'node:events';

const errors = [];
const queueMicrotask = globalThis.queueMicrotask;
globalThis.queueMicrotask = (callback) =>
  queueMicrotask(() => {
    try {
      callback();
    } catch (error) {
      errors.push(error);
    }
  });

export const warningBeforeProcessLoads = {
  async test() {
    const emitter = new EventEmitter();
    for (let i = 0; i <= EventEmitter.defaultMaxListeners; i++) {
      emitter.on('event', () => {});
    }
    await new Promise((resolve) => setTimeout(resolve, 0));
    if (errors.length > 0) throw errors[0];
  },
};
