// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual, deepStrictEqual } from 'node:assert';

export const testHardwareConcurrency = {
  async test() {
    strictEqual(navigator.hardwareConcurrency, 1);
  },
};

export const testUserAgent = {
  async test() {
    strictEqual(navigator.userAgent, 'Cloudflare-Workers');
  },
};

export const testLanguage = {
  async test() {
    strictEqual(navigator.language, 'en');
    Object.defineProperty(navigator, 'language', { value: 'tr' });
    strictEqual(navigator.language, 'tr');
  },
};

export const testLanguages = {
  async test() {
    deepStrictEqual(navigator.languages, ['en']);
  },
};
