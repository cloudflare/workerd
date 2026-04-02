// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { connect } from 'cloudflare:sockets';
import { strictEqual } from 'assert';

export const connectHandler = {
  async test() {
    // Check that the connect handler can send a message through a socket
    const socket = connect('localhost:8081');
    await socket.opened;
    const dec = new TextDecoder();
    let result = '';
    for await (const chunk of socket.readable) {
      result += dec.decode(chunk, { stream: true });
    }
    result += dec.decode();
    strictEqual(result, 'hello');
    await socket.closed;
  },
};

export const connectHandlerProxy = {
  async test() {
    // Check that we can get a message proxied through a connect handler. This call connects us with
    // an instance of Server, which serves as a proxy for an instance of OtherServer, as defined in
    // connect-handler-test-proxy.js.
    const socket = connect('localhost:8082');
    await socket.opened;
    const dec = new TextDecoder();
    let result = '';
    for await (const chunk of socket.readable) {
      result += dec.decode(chunk, { stream: true });
    }
    result += dec.decode();
    strictEqual(result, 'hello-from-endpoint');
    await socket.closed;
  },
};

export default {
  async connect(socket) {
    const enc = new TextEncoder();
    let writer = socket.writable.getWriter();
    await writer.write(enc.encode('hello'));
    await writer.close();
  },
};
