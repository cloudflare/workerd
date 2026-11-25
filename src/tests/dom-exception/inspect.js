// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// util.inspect(), which console.log() uses (ledger #4). V8's implementation
// renders as Node renders its own DOMException.

import { strictEqual } from 'node:assert';
import { inspect } from 'node:util';
import { usingV8Impl, setToStringTag } from 'which-impl';
import { kLegacyCodes } from 'helpers';

export const inspectInstance = {
  test() {
    const error = new DOMException('m', 'AbortError');
    error.stack = 'AbortError: m\n    at somewhere';
    const expected = usingV8Impl
      ? 'DOMException [AbortError]: m\n    at somewhere'
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
      usingV8Impl
        ? '{\n  error: DOMException [AbortError]: m\n      at somewhere\n}'
        : '{ error: [DOMException] }'
    );
  },
};

export const inspectPrototype = {
  test() {
    const constants = kLegacyCodes
      .map(([constant, value]) => `  ${constant}: ${value}`)
      .join(',\n');
    const tag = setToStringTag ? ' [DOMException]' : '';
    // V8's prototype is not a DOMException, so its brand-checked getters
    // throw, and inspect() renders it as an object.
    const accessors =
      '  name: [Getter],\n  message: [Getter],\n  code: [Getter],\n';
    const expected = usingV8Impl
      ? `[object DOMException] {\n${accessors}${constants}\n}`
      : `[Error${tag}] {\n${constants}\n}`;
    strictEqual(inspect(DOMException.prototype), expected);
  },
};
