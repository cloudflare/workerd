// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Same worker as ../udp-connect/index.mjs, run instead under the typescript_implemented_streams
// compat flag: this config exercises JsReadableStream::fromPull()'s TypeScript arm rather than
// its legacy C++ arm. See that test for the full behavior description.

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
