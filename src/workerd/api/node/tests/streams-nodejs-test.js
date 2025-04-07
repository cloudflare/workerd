import { Duplex, Readable, Writable } from 'node:stream';
import { strictEqual, deepStrictEqual } from 'node:assert';

export const testStreamDuplexDestroy = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const duplex = new Duplex({
      read() {},
      write(chunk, enc, cb) {
        cb();
      },
    });

    duplex.cork();
    duplex.write('foo', (err) => {
      strictEqual(err.code, 'ERR_STREAM_DESTROYED');
      resolve();
    });
    duplex.destroy();
    await promise;
  },
};

// Prevents stream unexpected pause when highWaterMark set to 0
// Ref: https://github.com/nodejs/node/commit/50695e5de14ccd8255537972181bbc9a1f44368e
export const testStreamsHighwatermark = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const res = [];
    const r = new Readable({
      read() {},
    });
    const w = new Writable({
      highWaterMark: 0,
      write(chunk, encoding, callback) {
        res.push(chunk.toString());
        callback();
      },
    });

    r.pipe(w);
    r.push('a');
    r.push('b');
    r.push('c');
    r.push(null);

    r.on('end', () => {
      deepStrictEqual(res, ['a', 'b', 'c']);
      resolve();
    });
    await promise;
  },
};
