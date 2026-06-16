// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-369: process abort via
// prototype-polluted getter during Request host-object deserialization.
// Pre-fix: V8_Fatal("Invoke in DisallowJavascriptExecutionScope") -> abort.
// Post-fix: structuredClone succeeds normally.

import assert from 'node:assert';

function withProtoGetter(prop, fn) {
  const saved = Object.getOwnPropertyDescriptor(Object.prototype, prop);
  Object.defineProperty(Object.prototype, prop, {
    configurable: true,
    get: () => undefined,
  });
  try {
    fn();
  } finally {
    if (saved) {
      Object.defineProperty(Object.prototype, prop, saved);
    } else {
      // eslint-disable-next-line
      delete Object.prototype[prop];
    }
  }
}

export const requestPrototypePollutionRedirect = {
  test() {
    withProtoGetter('redirect', () => {
      const cloned = structuredClone(new Request('https://example.com/'));
      assert.strictEqual(cloned.url, 'https://example.com/');
      assert.ok(cloned instanceof Request);
    });
  },
};

export const requestPrototypePollutionMethod = {
  test() {
    withProtoGetter('method', () => {
      const cloned = structuredClone(new Request('https://example.com/'));
      assert.strictEqual(cloned.url, 'https://example.com/');
      assert.strictEqual(cloned.method, 'GET');
    });
  },
};

export const requestPrototypePollutionSignal = {
  test() {
    withProtoGetter('signal', () => {
      const cloned = structuredClone(new Request('https://example.com/'));
      assert.strictEqual(cloned.url, 'https://example.com/');
      assert.ok(cloned instanceof Request);
    });
  },
};
