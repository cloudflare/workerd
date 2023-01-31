import { EventEmitter } from 'node:events';

export default {
  async fetch(request) {
    let res;
    const promise = new Promise((a) => res = a);

    const ee = new EventEmitter();
    ee.on('foo', () => {
      res(new Response('Hello World'));
    });

    setTimeout(() => ee.emit('foo'), 10);

    return promise;
  }
};
