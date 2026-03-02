// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import {
  deepStrictEqual,
  doesNotThrow,
  strictEqual,
  throws,
} from 'node:assert';

// Test that WebSocket constructor handles empty protocols array correctly.
// Per the WebSocket spec, an empty protocols array should be valid and equivalent
// to not specifying any protocols.
// See: https://github.com/cloudflare/workerd/issues/5822
export const emptyProtocolsArray = {
  async test() {
    const ws = new WebSocket('wss://example.com/', []);
    strictEqual(ws.url, 'wss://example.com/');
    strictEqual(ws.protocol, '');
    strictEqual(ws.readyState, WebSocket.CONNECTING);
    ws.close();
  },
};

// Test that a single protocol string works
export const singleProtocolString = {
  async test() {
    const ws = new WebSocket('wss://example.com/', 'chat');
    strictEqual(ws.url, 'wss://example.com/');
    ws.close();
  },
};

// Test that invalid protocol tokens still throw SyntaxError
export const invalidProtocolToken = {
  async test() {
    throws(
      () => new WebSocket('wss://example.com/', 'invalid protocol with spaces'),
      {
        name: 'SyntaxError',
      }
    );
  },
};

// Test that duplicate valid protocols throw SyntaxError
export const duplicateProtocols = {
  async test() {
    throws(() => new WebSocket('wss://example.com/', ['chat', 'chat']), {
      name: 'SyntaxError',
    });
  },
};

// Test that close() throws SyntaxError when reason exceeds 123 bytes (UTF-8).
// Per WHATWG WebSocket spec and RFC 6455 Section 5.5, the close frame body
// must not exceed 125 bytes (2-byte code + up to 123 bytes of reason).
export const closeReasonTooLong = {
  async test() {
    const ws = new WebSocket('wss://example.com/');
    // 124 ASCII bytes => 124 UTF-8 bytes, which exceeds the 123-byte limit.
    const longReason = 'a'.repeat(124);
    throws(() => ws.close(1000, longReason), {
      name: 'SyntaxError',
    });
    ws.close();
  },
};

// Test that close() accepts a reason of exactly 123 bytes.
export const closeReasonExact123Bytes = {
  async test() {
    const ws = new WebSocket('wss://example.com/');
    const reason123 = 'a'.repeat(123);
    doesNotThrow(() => ws.close(1000, reason123));
  },
};

// Test that close() counts UTF-8 bytes, not characters.
// U+00E9 (é) is 2 bytes in UTF-8. 62 of them = 124 bytes > 123 limit.
export const closeReasonMultibyteExceeds = {
  async test() {
    const ws = new WebSocket('wss://example.com/');
    const multibyteReason = '\u00e9'.repeat(62); // 62 chars × 2 bytes = 124 bytes
    throws(() => ws.close(1000, multibyteReason), {
      name: 'SyntaxError',
    });
    ws.close();
  },
};

// Test that close() replaces lone surrogates with U+FFFD per the USVString spec.
// This is tested end-to-end by the WPT Close-reason-unpaired-surrogates.any.js test.
// Here we verify the CloseEvent constructor also applies USVString conversion.
export const closeReasonUSVString = {
  async test() {
    // CloseEvent.reason is typed as USVString per the HTML spec,
    // so lone surrogates must be replaced with U+FFFD.
    const evt = new CloseEvent('close', { code: 1000, reason: '\uD807' });
    strictEqual(
      evt.reason,
      '\uFFFD',
      'CloseEvent reason should replace lone surrogate with U+FFFD'
    );

    // Multiple surrogates in a string.
    const evt2 = new CloseEvent('close', {
      code: 1000,
      reason: 'hello\uD800world\uDC00end',
    });
    strictEqual(
      evt2.reason,
      'hello\uFFFDworld\uFFFDend',
      'CloseEvent reason should replace each lone surrogate with U+FFFD'
    );

    // Properly paired surrogates should pass through unchanged.
    const evt3 = new CloseEvent('close', {
      code: 1000,
      reason: '\uD83D\uDE00',
    });
    strictEqual(
      evt3.reason,
      '\uD83D\uDE00',
      'Properly paired surrogates should not be replaced'
    );
  },
};

// Test that close() with code 1000 succeeds (Normal Closure).
export const closeCode1000Succeeds = {
  async test() {
    const ws = new WebSocket('wss://example.com/');
    doesNotThrow(() => ws.close(1000));
  },
};

// Test that close() with codes in the 3000-4999 range succeeds.
export const closeCode3000To4999Succeeds = {
  async test() {
    for (const code of [3000, 3500, 4000, 4999]) {
      const ws = new WebSocket('wss://example.com/');
      doesNotThrow(() => ws.close(code));
    }
  },
};

// Test that close() rejects codes outside the spec-allowed set.
// Per WHATWG WebSocket spec, only 1000 and 3000-4999 are valid.
export const closeCodeSpecInvalid = {
  async test() {
    for (const code of [999, 1001, 1004, 1005, 1006, 1015, 2999, 5000]) {
      const ws = new WebSocket('wss://example.com/');
      throws(
        () => ws.close(code),
        { name: 'InvalidAccessError' },
        `close(${code}) should throw InvalidAccessError`
      );
      ws.close();
    }
  },
};

// Test that binaryType defaults to "blob" when websocket_standard_binary_type flag is on.
export const binaryTypeDefaultsToBlob = {
  async test() {
    const ws = new WebSocket('wss://example.com/');
    strictEqual(ws.binaryType, 'blob');
    ws.close();
  },
};

// Test that binaryType on WebSocketPair also defaults to "blob".
export const binaryTypeWebSocketPairDefault = {
  async test() {
    const pair = new WebSocketPair();
    strictEqual(pair[0].binaryType, 'blob');
    strictEqual(pair[1].binaryType, 'blob');
    // WebSocketPair sockets require accept() before close(), but we only
    // need to verify the default value so no cleanup is needed.
  },
};

// Test that binaryType setter accepts "arraybuffer" and "blob".
export const binaryTypeSetterValid = {
  async test() {
    const ws = new WebSocket('wss://example.com/');
    strictEqual(ws.binaryType, 'blob');

    ws.binaryType = 'arraybuffer';
    strictEqual(ws.binaryType, 'arraybuffer');

    ws.binaryType = 'blob';
    strictEqual(ws.binaryType, 'blob');
    ws.close();
  },
};

// Test that binaryType setter silently ignores invalid values per spec.
export const binaryTypeSetterInvalid = {
  async test() {
    const ws = new WebSocket('wss://example.com/');
    strictEqual(ws.binaryType, 'blob');

    ws.binaryType = 'notBlobOrArrayBuffer';
    strictEqual(ws.binaryType, 'blob');

    ws.binaryType = '';
    strictEqual(ws.binaryType, 'blob');

    ws.binaryType = 'arraybuffer';
    strictEqual(ws.binaryType, 'arraybuffer');

    ws.binaryType = 'BLOB';
    strictEqual(ws.binaryType, 'arraybuffer');
    ws.close();
  },
};

// Test that binary messages are delivered as Blob when binaryType is "blob".
export const binaryMessageDeliveredAsBlob = {
  async test() {
    const pair = new WebSocketPair();
    const [client, server] = pair;
    server.accept();

    const data = new Uint8Array([1, 2, 3, 4, 5]);
    const received = new Promise((resolve) => {
      server.addEventListener('message', (event) => resolve(event.data));
    });

    client.accept();
    client.send(data);

    const msg = await received;
    strictEqual(msg instanceof Blob, true);
    deepStrictEqual(new Uint8Array(await msg.arrayBuffer()), data);

    client.close();
    server.close();
  },
};

// Test that binary messages are delivered as ArrayBuffer when binaryType is "arraybuffer".
export const binaryMessageDeliveredAsArrayBuffer = {
  async test() {
    const pair = new WebSocketPair();
    const [client, server] = pair;
    server.accept();
    server.binaryType = 'arraybuffer';

    const data = new Uint8Array([10, 20, 30]);
    const received = new Promise((resolve) => {
      server.addEventListener('message', (event) => resolve(event.data));
    });

    client.accept();
    client.send(data);

    const msg = await received;
    strictEqual(msg instanceof ArrayBuffer, true);
    deepStrictEqual(new Uint8Array(msg), data);

    client.close();
    server.close();
  },
};

// Test that switching binaryType mid-stream changes delivery format.
export const binaryTypeSwitchMidStream = {
  async test() {
    const pair = new WebSocketPair();
    const [client, server] = pair;
    server.accept();
    client.accept();

    // Default is "blob" with the compat flag.
    strictEqual(server.binaryType, 'blob');

    // First message: delivered as Blob.
    {
      const received = new Promise((resolve) => {
        server.addEventListener('message', (event) => resolve(event.data), {
          once: true,
        });
      });
      client.send(new Uint8Array([1]));
      const msg = await received;
      strictEqual(msg instanceof Blob, true);
    }

    // Switch to arraybuffer.
    server.binaryType = 'arraybuffer';

    // Second message: delivered as ArrayBuffer.
    {
      const received = new Promise((resolve) => {
        server.addEventListener('message', (event) => resolve(event.data), {
          once: true,
        });
      });
      client.send(new Uint8Array([2]));
      const msg = await received;
      strictEqual(msg instanceof ArrayBuffer, true);
      deepStrictEqual(new Uint8Array(msg), new Uint8Array([2]));
    }

    client.close();
    server.close();
  },
};
