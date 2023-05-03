import { EventEmitter } from 'node:events';
import { Buffer } from 'node:buffer';
import {
  ok,
  deepStrictEqual,
  throws,
} from 'node:assert';
import {
  callbackify,
  promisify,
  format,
} from 'node:util';

import { default as path } from 'node:path';

console.log(path.resolve('a', 'b', 'c'));
console.log(path.basename('/a/b/c/d.foo'));
console.log(path.extname('/a/b/c/d.foo'));

// Note that the path.win32 variants of the path API are not yet implemented.
// While workerd is capable of running on Windows, we assume that the environment
// is POSIX-like for now.
throws(() => path.win32.resolve('a', 'b', 'c'), {
  message: 'path.win32.resolve() is not implemented.'
});

// Callback function
function doSomething(a, cb) {
  setTimeout(() => cb(null, a), 1);
}

// Async function
async function promiseSomething(a) {
  await scheduler.wait(1);
  return a;
}

const promisified = promisify(doSomething);
const callbackified = callbackify(promiseSomething);

export default {
  async fetch(request) {
    let res;
    const promise = new Promise((a) => res = a);

    // Util promisify/callbackify
    console.log(await promisified(321));

    callbackified(123, (err, val) => {
      console.log(err, val);
    });

    // The events module...
    const ee = new EventEmitter();
    ee.on('foo', () => {

      // The assertion module...
      ok(true);
      deepStrictEqual({
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
      throws(() => {
        // util.format
        throw new Error(format('%s', 'boom'));
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
