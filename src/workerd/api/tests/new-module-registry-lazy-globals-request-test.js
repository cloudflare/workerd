// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Nothing here touches process or Buffer, or imports a module that loads them, before the test
// runs, so the test makes the first access to both.

function check(value, message) {
  if (!value) throw new Error(message);
}

export const firstAccessInRequest = {
  async test() {
    let ran = false;
    queueMicrotask(() => {
      ran = true;
    });

    const process = globalThis.process;
    check(!ran, 'the first process access ran a pending microtask');
    const Buffer = globalThis.Buffer;
    check(!ran, 'the first Buffer access ran a pending microtask');

    await Promise.resolve();
    check(ran, 'the microtask did not run');

    check(process === (await import('node:process')).default, 'wrong process');
    check(Buffer === (await import('node:buffer')).Buffer, 'wrong Buffer');
  },
};
