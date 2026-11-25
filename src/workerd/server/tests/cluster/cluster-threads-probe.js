// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The Worker behind the datagram socket of cluster-threads.wd-test. Only an
// instance's first thread serves a datagram socket, and a service binding
// reaches the Worker of the same thread, so each datagram is answered with
// what `main` answers `/thread` there: the first thread's isolate.

export default {
  async connect(socket, env) {
    const writer = socket.writable.getWriter();
    for await (const _ of socket.readable) {
      const where = await env.MAIN.fetch('http://main/thread');
      await writer.write(
        new Datagram(new Uint8Array(await where.arrayBuffer()))
      );
    }
  },
};
