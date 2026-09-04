// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Test worker for the UDP `connect()` handler. Each datagram flow (grouped by peer address) gets
// its own `connect()` invocation. The first datagram received on a flow gets a reply prefixed with
// `first:<protocol>:`, so the test can assert `socket.protocol` from the wire. Every datagram after
// that just gets echoed back verbatim (prefixed with `echo:`), so the test can assert that chunk
// boundaries are preserved regardless of datagram size.

const encoder = new TextEncoder();

export default {
  async connect(socket) {
    const reader = socket.readable.getReader();
    const writer = socket.writable.getWriter();
    let first = true;
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      const bytes = value.data;
      if (first) {
        first = false;
        const prefix = encoder.encode(`first:${socket.protocol}:`);
        const combined = new Uint8Array(prefix.length + bytes.length);
        combined.set(prefix, 0);
        combined.set(bytes, prefix.length);
        await writer.write(new Datagram(combined));
      } else {
        const prefix = encoder.encode('echo:');
        const combined = new Uint8Array(prefix.length + bytes.length);
        combined.set(prefix, 0);
        combined.set(bytes, prefix.length);
        await writer.write(new Datagram(combined));
      }
    }
  },
};
