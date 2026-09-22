// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared machinery for the node:stream suite.

import { strictEqual } from 'node:assert';

export function once(emitter, event) {
  return new Promise((resolve) => emitter.once(event, resolve));
}

// Runs `fn` while recording every uncaught exception and unhandled
// rejection the isolate reports, waits a beat for late ones, and fails if
// any leaked: the assertion that nothing escapes the adapters.
export async function withUncaughtGuard(fn) {
  const leaked = [];
  const onError = (event) => {
    leaked.push(event.error ?? event.message);
    event.preventDefault();
  };
  const onRejection = (event) => {
    leaked.push(event.reason);
    event.preventDefault();
  };
  globalThis.addEventListener('error', onError);
  globalThis.addEventListener('unhandledrejection', onRejection);
  try {
    await fn();
    await scheduler.wait(20);
  } finally {
    globalThis.removeEventListener('error', onError);
    globalThis.removeEventListener('unhandledrejection', onRejection);
  }
  strictEqual(leaked.length, 0, `leaked: ${leaked.join(', ')}`);
}
