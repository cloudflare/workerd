// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The Worker behind the datagram socket of `front-config.capnp`. Only the server's first thread
// serves a datagram socket, and a service binding reaches the Worker of the same thread, so each
// datagram is answered with what the `front` of the first thread answers `/where`: its isolate.

export default {
  async connect(socket, env) {
    const writer = socket.writable.getWriter();
    for await (const _ of socket.readable) {
      const where = await env.FRONT.fetch('http://front/where');
      await writer.write(
        new Datagram(new Uint8Array(await where.arrayBuffer()))
      );
    }
  },
};
