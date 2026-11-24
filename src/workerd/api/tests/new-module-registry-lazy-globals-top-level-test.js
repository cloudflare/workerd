// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The first access to process and Buffer happens here, during top-level evaluation.

let ran = false;
queueMicrotask(() => {
  ran = true;
});
const process = globalThis.process;
const ranAfterProcess = ran;
const Buffer = globalThis.Buffer;
const ranAfterBuffer = ran;

function check(value, message) {
  if (!value) throw new Error(message);
}

export const firstAccessAtTopLevel = {
  async test() {
    check(!ranAfterProcess, 'the first process access ran a pending microtask');
    check(!ranAfterBuffer, 'the first Buffer access ran a pending microtask');
    check(ran, 'the microtask did not run');
    check(process === (await import('node:process')).default, 'wrong process');
    check(Buffer === (await import('node:buffer')).Buffer, 'wrong Buffer');
  },
};
