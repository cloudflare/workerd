// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'assert';

// An inbound socket is half-open: the handler can still reply after the peer
// has finished sending. The client half-closes before reading, and its `closed`
// settles once it has read to EOF.
export const replyAfterPeerHalfClose = {
  async test(ctrl, env) {
    const socket = env.SELF.connect('example.com:1');
    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode('ping'));
    await writer.close();
    const dec = new TextDecoder();
    let result = '';
    for await (const chunk of socket.readable) {
      result += dec.decode(chunk, { stream: true });
    }
    result += dec.decode();
    strictEqual(result, 'echo:ping');
    await socket.closed;
  },
};

// Reads everything the peer sends, then replies after the peer has half-closed.
export default {
  async connect(socket) {
    const dec = new TextDecoder();
    let received = '';
    for await (const chunk of socket.readable) {
      received += dec.decode(chunk, { stream: true });
    }
    received += dec.decode();
    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode(`echo:${received}`));
    await writer.close();
  },
};
