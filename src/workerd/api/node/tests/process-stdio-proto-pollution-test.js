// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Every service in process-stdio-proto-pollution-test.wd-test runs this module in its own
// isolate with a different `key` binding. This module must not import node:process, node:fs,
// node:stream or node:console, so that the stdio module graph is first evaluated after
// Object.prototype has gained a property named after a property descriptor field.
import { strictEqual, ok } from 'node:assert';

const envAtStartup = process.env;

export default {
  async test(ctrl, env) {
    ok(envAtStartup);
    const key = env.key;
    Object.defineProperty(Object.prototype, key, {
      __proto__: null,
      writable: true,
      configurable: true,
    });
    try {
      strictEqual(process.stdout.fd, 1);
      strictEqual(process.stderr.fd, 2);
      strictEqual(process.stdin.fd, 0);
      ok(process.stdout.write(`written with Object.prototype.${key}\n`));
      await import('node:stream');
    } finally {
      Reflect.deleteProperty(Object.prototype, key);
    }
  },
};
