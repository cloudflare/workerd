// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'node:assert';

// 'echo:*' echoes with an explicit read/write loop (a same-socket pipeTo over
// an in-process pipe would splice, so the peer's write could not complete
// before it reads). 'proxy:*' pipes the inbound socket to an echo connection
// and closes both once the pipes finish, while the pipe's close of the inbound
// writable may still be in flight.
export default {
  async connect(socket, env) {
    const { localAddress } = await socket.opened;
    void socket.closed.catch(() => {});
    if (localAddress.startsWith('echo:')) {
      const reader = socket.readable.getReader();
      const writer = socket.writable.getWriter();
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        await writer.write(value);
      }
      await writer.close();
      await socket.close();
      return;
    }
    const target = env.SELF.connect('echo:1', { allowHalfOpen: true });
    void target.closed.catch(() => {});
    await Promise.allSettled([
      socket.readable.pipeTo(target.writable),
      target.readable.pipeTo(socket.writable),
    ]);
    await Promise.allSettled([socket.close(), target.close()]);
  },
};

export const proxiedEchoThenClose = {
  async test(ctrl, env) {
    const socket = env.SELF.connect('proxy:1');
    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode('hello'));
    const reader = socket.readable.getReader();
    const { value } = await reader.read();
    strictEqual(new TextDecoder().decode(value), 'hello');
    reader.releaseLock();
    await writer.close();
    for await (const chunk of socket.readable) void chunk;
    // Let the handlers' closes and any late rejection surface before exit.
    await scheduler.wait(300);
  },
};
