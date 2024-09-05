// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { createRequire } from 'node:module';
import { ok, strictEqual, throws } from 'node:assert';

export const doTheTest = {
  async test() {
    const require = createRequire('/');
    ok(typeof require === 'function');

    const foo = require('foo');
    const bar = require('bar');
    const baz = require('baz');
    const qux = require('worker/qux');

    strictEqual(foo.default, 1);
    strictEqual(bar, 2);
    strictEqual(baz, 3);
    strictEqual(qux, '4');

    const assert = await import('node:assert');
    const required = require('node:assert');
    strictEqual(assert, required);

    throws(() => require('invalid'), {
      message: 'Module evaluation did not complete synchronously.',
    });

    throws(() => require('does not exist'));
    throws(() => createRequire('not a valid path'), {
      message: /The argument must be a file URL object/,
    });
    throws(() => createRequire(new URL('http://example.org')), {
      message: /The argument must be a file URL object/,
    });

    // TODO(soon): Later when we when complete the new module registry, query strings
    // and hash fragments will be allowed when the new registry is being used.
    throws(() => createRequire('file://test?abc'), {
      message:
        'The specifier must not have query string parameters or hash fragments.',
    });
    throws(() => createRequire('file://test#123'), {
      message:
        'The specifier must not have query string parameters or hash fragments.',
    });

    // These should not throw...
    createRequire('file:///');
    createRequire('file:///tmp');
    createRequire(new URL('file:///'));
  },
};
