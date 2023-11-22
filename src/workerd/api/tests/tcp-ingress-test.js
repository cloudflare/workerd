import { connect } from 'cloudflare:sockets';
import { ok, strictEqual } from 'assert';

export const newFunction = {
  async test() {
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

export default {
  connect({ inbound, cf }) {
    const enc = new TextEncoder();
    ok(inbound instanceof ReadableStream);
    strictEqual(typeof cf.clientIp, 'string');
    return ReadableStream.from([enc.encode('hello')]);
  },
};
