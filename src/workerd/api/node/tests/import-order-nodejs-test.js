import { default as fs } from 'fs';
import { default as net } from 'net';
import { ok, strictEqual } from 'assert';

export const testImportOrder = {
  async test() {
    const socket = new net.Socket();
    socket.connect(8787, '127.0.0.1');
    ok(socket.destroy);
    strictEqual(typeof socket.destroy, 'function');
    socket.destroy();
  },
};
