// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// util.inspect(), which console.log() uses (ledger #5).

import { strictEqual } from 'node:assert';
import { inspect } from 'node:util';
import { usingTsImpl, setToStringTag } from 'which-impl';
import { kLegacyCodes } from 'helpers';

export const inspectInstance = {
  test() {
    const error = new DOMException('m', 'AbortError');
    error.stack = 'AbortError: m\n    at somewhere';
    const expected = usingTsImpl
      ? 'AbortError: m\n    at somewhere'
      : 'DOMException {\n' +
        '  code: 20,\n' +
        "  name: 'AbortError',\n" +
        "  message: 'm',\n" +
        "  stack: 'AbortError: m\\n    at somewhere'\n" +
        '}';
    strictEqual(inspect(error), expected);
  },
};

export const inspectNested = {
  test() {
    const error = new DOMException('m', 'AbortError');
    error.stack = 'AbortError: m\n    at somewhere';
    strictEqual(
      inspect({ error }, { depth: 0 }),
      usingTsImpl ? '{ error: [AbortError: m] }' : '{ error: [DOMException] }'
    );
  },
};

export const inspectPrototype = {
  test() {
    const constants = kLegacyCodes
      .map(([constant, value]) => `  ${constant}: ${value}`)
      .join(',\n');
    const tag = setToStringTag ? ' [DOMException]' : '';
    // The TypeScript prototype is not a DOMException, so its brand-checked
    // getters must not be what inspect() renders.
    const expected = usingTsImpl
      ? `Object [DOMException] {}`
      : `[Error${tag}] {\n${constants}\n}`;
    strictEqual(inspect(DOMException.prototype), expected);
  },
};
