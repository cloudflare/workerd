// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Test worker for the UDP flow's bounded pending-bytes queue. The first datagram is a priming
// ping: it's read and echoed immediately, so the driver knows this handler is already running
// (past any isolate/module startup cost) before it fires the burst of datagrams meant to
// overflow the queue during the delay that follows.

export default {
  async connect(socket) {
    const reader = socket.readable.getReader();
    const writer = socket.writable.getWriter();

    const { value: ping } = await reader.read();
    await writer.write(ping);

    await scheduler.wait(300);

    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      await writer.write(value);
    }
  },
};
