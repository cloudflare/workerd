import {
  strictEqual,
} from 'node:assert';

export const dataUrl = {
  async test() {
    const resp = await fetch('data:text/plain,Hello%2C%20World!');
    strictEqual(resp.status, 200);
    strictEqual(resp.statusText, 'OK');
    strictEqual(await resp.text(), 'Hello, World!');
    strictEqual(resp.headers.get('content-type'), 'text/plain');
  }
};

export const base64DataUrl = {
  async test() {
    const resp = await fetch('  DATA:text/plain;a=\"b\";base64,\t\nSGVsbG8sIFdvcmxkIQ%3D%3D'  );
    strictEqual(resp.status, 200);
    strictEqual(resp.statusText, 'OK');
    strictEqual(await resp.text(), 'Hello, World!');
    strictEqual(resp.headers.get('content-type'), 'text/plain;a=b');
  }
};
