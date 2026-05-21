// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for snapshot semantics of resizable ArrayBuffers passed to APIs that
// go through jsg::asBytes().
//
// For resizable buffers, asBytes() returns a deep copy to prevent SIGSEGV from
// page decommit after resize(0). This test verifies that:
//
// 1. Data is captured at call time (not affected by post-call mutations)
// 2. The behavior is the same for resizable and non-resizable buffers
//
// In practice, WebSocket.send() (and similar APIs) consume the data
// synchronously during the call -- the WebSocket pump copies data to the pipe
// before returning to JS. So mutations after send() are never visible,
// regardless of whether the buffer is resizable or not.

import { strictEqual } from 'node:assert';

// Helper: send a buffer via WebSocket, mutate it, check what was received.
async function sendMutateReceive(buffer, initialText, mutatedText) {
  const pair = new WebSocketPair();
  const [client, server] = pair;
  server.accept();
  server.binaryType = 'arraybuffer';

  const received = new Promise((resolve) => {
    server.addEventListener('message', (e) => resolve(e.data));
  });

  client.accept();

  // Write initial data
  const view = new Uint8Array(buffer);
  new TextEncoder().encodeInto(initialText, view);

  // Send the buffer — this calls asBytes() internally
  client.send(buffer);

  // Mutate AFTER send — this should NOT affect the sent data because the
  // WebSocket pump copies data to the pipe synchronously during send().
  new TextEncoder().encodeInto(mutatedText, view);

  // Wait for the message to be delivered through the WebSocket pipe
  const msg = await received;

  client.close();
  server.close();

  return new TextDecoder().decode(msg);
}

// Non-resizable buffer: data is captured at send() time.
// Even though asBytes() returns a live view into the BackingStore, the
// WebSocket pump copies data to the pipe synchronously, so post-send
// mutations are not visible.
export const nonResizableBufferSnapshot = {
  async test() {
    const ab = new ArrayBuffer(7);  // non-resizable
    const text = await sendMutateReceive(ab, 'initial', 'CHANGED');
    strictEqual(text, 'initial',
      'non-resizable: data should be captured at send() time');
  },
};

// Resizable buffer: data is captured at send() time via deep copy.
// asBytes() copies the data defensively (to prevent SIGSEGV from resize(0)
// decommitting pages). The result is the same as non-resizable: the sent
// data reflects the buffer content at the time of the send() call.
export const resizableBufferSnapshot = {
  async test() {
    const ab = new ArrayBuffer(7, { maxByteLength: 16 });  // resizable
    const text = await sendMutateReceive(ab, 'initial', 'CHANGED');
    strictEqual(text, 'initial',
      'resizable: data should be captured at send() time (deep copy)');
  },
};

// Resizable buffer that was already resized down: asBytes() should handle
// the current (smaller) size correctly, not the max reservation size.
export const resizableBufferAfterShrink = {
  async test() {
    const ab = new ArrayBuffer(16, { maxByteLength: 32 });
    const view = new Uint8Array(ab);
    new TextEncoder().encodeInto('hello world12345', view);

    // Shrink to 5 bytes
    ab.resize(5);

    const pair = new WebSocketPair();
    const [client, server] = pair;
    server.accept();
    server.binaryType = 'arraybuffer';

    const received = new Promise((resolve) => {
      server.addEventListener('message', (e) => resolve(e.data));
    });

    client.accept();
    client.send(ab);

    const msg = await received;
    const text = new TextDecoder().decode(msg);
    strictEqual(text, 'hello',
      'resizable after shrink: should send only the current (5-byte) content');
    strictEqual(msg.byteLength, 5,
      'resizable after shrink: sent length should be current size, not max');

    client.close();
    server.close();
  },
};
