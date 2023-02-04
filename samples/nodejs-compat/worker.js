import { EventEmitter } from 'node:events';
import { Buffer } from 'node:buffer';
import * as assert from 'node:assert';

export default {
  async fetch(request) {
    let res;
    const promise = new Promise((a) => res = a);

    // The events module...
    const ee = new EventEmitter();
    ee.on('foo', () => {

      // The assertion module...
      assert.ok(true);
      assert.deepStrictEqual({
        a: {
          b: new Set([1,2,3]),
          c: [
            new Map([['a','b']])
          ]
        }
      }, {
        a: {
          b: new Set([1,2,3]),
          c: [
            new Map([['a','b']])
          ]
        }
      });
      assert.throws(() => {
        throw new Error('boom');
      }, new Error('boom'));

      // The buffer module...
      const buffer = Buffer.concat([Buffer.from('Hello '), Buffer.from('There')], 12);
      buffer.fill(Buffer.from('!!'), 11);

      res(new Response(buffer));
    });

    setTimeout(() => ee.emit('foo'), 10);

    return promise;
  }
};
