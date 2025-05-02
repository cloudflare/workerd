import { strictEqual } from 'node:assert';

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
