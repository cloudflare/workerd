// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as foo from 'foo';
import * as baz from 'baz';
import * as vm from 'node:vm';
import { strictEqual } from 'node:assert';
import * as cjs from 'cjs';

try {
  await import('bar');
  throw new Error('bar should not have been imported');
} catch {
  console.log('tried to import bar which does not exist');
};

console.log(foo, baz, vm);

export default {
  async fetch(req, env) {
    strictEqual(1 + 1, 2);
    return new Response("Hello World\n");
  }
};
