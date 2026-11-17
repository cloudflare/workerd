// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This test must not import node:process, node:fs or node:console, so that the stdio
// streams are first built inside a request, through the accessors on the global.
import { strictEqual, ok } from 'node:assert';

const envAtStartup = process.env;

export default {
  async test() {
    ok(envAtStartup);
    const stdout = process.stdout;
    strictEqual(stdout.fd, 1);
    strictEqual(process.stderr.fd, 2);
    strictEqual(process.stdin.fd, 0);
    strictEqual(process.stdin.read(), null);
    ok(stdout.write('written from a request\n'));

    // The same streams as node:process exports.
    const processMod = await import('node:process');
    strictEqual(processMod.stdout, stdout);
    strictEqual(processMod.default, process);
    strictEqual(processMod.default.stdout, stdout);
  },
};
