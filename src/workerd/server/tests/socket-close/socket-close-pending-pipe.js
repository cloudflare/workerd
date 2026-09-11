// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual, deepStrictEqual } from 'node:assert';

// 'echo:*' echoes with an explicit read/write loop (a same-socket pipeTo over
// an in-process pipe would splice, so the peer's write could not complete
// before it reads). 'proxy:*' pipes the inbound socket to an echo connection
// and closes both once the pipes finish, while the pipe's close of the inbound
// writable may still be in flight. Assertion failures here surface as uncaught
// exceptions in workerd's log, which socket-close-test.mjs checks.
export default {
  async connect(socket, env) {
    const { localAddress } = await socket.opened;
    if (localAddress.startsWith('echo:')) {
      const writer = socket.writable.getWriter();
      for await (const chunk of socket.readable) {
        await writer.write(chunk);
      }
      await writer.close();
      await socket.close();
      await socket.closed;
      return;
    }
    const target = env.SELF.connect('echo:1', { allowHalfOpen: true });
    const pipes = await Promise.allSettled([
      socket.readable.pipeTo(target.writable),
      target.readable.pipeTo(socket.writable),
    ]);
    deepStrictEqual(
      pipes.map((r) => r.status),
      ['fulfilled', 'fulfilled']
    );
    const closes = await Promise.allSettled([socket.close(), target.close()]);
    deepStrictEqual(
      closes.map((r) => r.status),
      ['fulfilled', 'fulfilled']
    );
    await Promise.all([socket.closed, target.closed]);
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
    let rest = 0;
    for await (const chunk of socket.readable) rest += chunk.byteLength;
    strictEqual(rest, 0);
    // Let the handlers' closes and any late rejection surface before exit.
    await scheduler.wait(300);
  },
};
