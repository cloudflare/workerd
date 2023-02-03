import { EventEmitter } from 'node:events';
import { Buffer } from 'node:buffer';

export default {
  async fetch(request) {
    let res;
    const promise = new Promise((a) => res = a);

    const ee = new EventEmitter();
    ee.on('foo', () => {

      const buffer = Buffer.concat([Buffer.from('Hello '), Buffer.from('There')], 12);
      buffer.fill(Buffer.from('!!'), 11);

      res(new Response(buffer));
    });

    setTimeout(() => ee.emit('foo'), 10);

    return promise;
  }
};
