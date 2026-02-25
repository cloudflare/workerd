import { connect } from 'cloudflare:sockets';
import { ok, strictEqual } from 'assert';

export const connectHandler = {
  async test() {
    // Check that the connect handler can send a message through a socket
    const socket = connect('localhost:8081');
    await socket.opened;
    const dec = new TextDecoder();
    let result = '';
    for await (const chunk of socket.readable) {
      dec.decode(chunk, { stream: true });
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
    // an instance of Server, which serves as a proxy for an instance of OtherServer.
    const socket = connect('localhost:8082');
    await socket.opened;
    const dec = new TextDecoder();
    let result = '';
    for await (const chunk of socket.readable) {
      dec.decode(chunk, { stream: true });
      result += dec.decode(chunk, { stream: true });
    }
    result += dec.decode();
    strictEqual(result, 'hello-from-endpoint');
    await socket.closed;
  },
};

export default {
  connect({ socket, cf }) {
    const enc = new TextEncoder();
    strictEqual(typeof cf.clientIp, 'string');
    socket.writable.getWriter().write(enc.encode('hello'));
  },
};
