import http2 from 'node:http2';
import { strictEqual, deepStrictEqual, throws } from 'node:assert';

export const http2Test = {
  test() {
    strictEqual(typeof http2, 'object');
    strictEqual(typeof http2.constants, 'object');
    strictEqual(typeof http2.createSecureServer, 'function');
    strictEqual(typeof http2.createServer, 'function');
    strictEqual(typeof http2.Http2ServerRequest, 'function');
    strictEqual(typeof http2.Http2ServerResponse, 'function');
    strictEqual(typeof http2.connect, 'function');
    strictEqual(typeof http2.getDefaultSettings, 'function');
    strictEqual(typeof http2.getPackedSettings, 'function');
    strictEqual(typeof http2.getUnpackedSettings, 'function');
    strictEqual(typeof http2.performServerHandshake, 'function');
    strictEqual(typeof http2.sensitiveHeaders, 'symbol');
  },
};
