// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Simulates the Hyperdrive RPC worker's TCP side: replies with the authority the socket was
// opened against, then echoes everything written to it.
export default {
  async connect(socket) {
    const { localAddress } = await socket.opened;
    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode(`${localAddress}\n`));
    for await (const chunk of socket.readable) {
      await writer.write(chunk);
    }
    await writer.close();
  },
};
