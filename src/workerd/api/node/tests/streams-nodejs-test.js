import { Duplex, Readable, Writable } from 'node:stream';
import { strictEqual, deepStrictEqual, ok } from 'node:assert';
import { mock } from 'node:test';

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

// Tests are taken from
// https://github.com/nodejs/node/blob/9cc019575961ad9fcc18883993c3f9056699908d/test/parallel/test-stream-readable-data.js

export const testStreamReadableData = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const readable = new Readable({
      read() {},
    });

    const onReadableFn = mock.fn();
    readable.setEncoding('utf8');
    readable.on('readable', onReadableFn);
    readable.removeListener('readable', onReadableFn);
    readable.on('end', resolve);

    const onDataFn = mock.fn();
    queueMicrotask(function () {
      readable.on('data', onDataFn);
      readable.push('hello');

      queueMicrotask(() => {
        readable.push(null);
      });
    });

    await promise;
    strictEqual(onDataFn.mock.callCount(), 1);
    strictEqual(onReadableFn.mock.callCount(), 0);
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/9cc019575961ad9fcc18883993c3f9056699908d/test/parallel/test-stream-readable-ended.js
export const testStreamReadableEnded = {
  async test() {
    // basic
    {
      // Find it on Readable.prototype
      ok(Object.hasOwn(Readable.prototype, 'readableEnded'));
    }

    // event
    {
      const { promise, resolve } = Promise.withResolvers();
      const readable = new Readable();

      readable._read = () => {
        // The state ended should start in false.
        strictEqual(readable.readableEnded, false);
        readable.push('asd');
        strictEqual(readable.readableEnded, false);
        readable.push(null);
        strictEqual(readable.readableEnded, false);
      };

      const onDataFn = mock.fn(() => {
        strictEqual(readable.readableEnded, false);
      });

      readable.on('end', () => {
        strictEqual(readable.readableEnded, true);
        strictEqual(onDataFn.mock.callCount(), 1);
        resolve();
      });

      readable.on('data', onDataFn);

      await promise;
    }

    // Verifies no `error` triggered on multiple .push(null) invocations
    {
      const { promise, resolve, reject } = Promise.withResolvers();
      const readable = new Readable();

      readable.on('readable', () => {
        readable.read();
      });
      readable.on('error', reject);
      readable.on('end', resolve);

      readable.push('a');
      readable.push(null);
      readable.push(null);

      await promise;
    }
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/9cc019575961ad9fcc18883993c3f9056699908d/test/parallel/test-stream-readable-hwm-0.js
export const testStreamReadableHwm0 = {
  async test() {
    // This test ensures that Readable stream will call _read() for streams
    // with highWaterMark === 0 upon .read(0) instead of just trying to
    // emit 'readable' event.
    const { promise, resolve } = Promise.withResolvers();
    const onReadFn = mock.fn();
    const r = new Readable({
      // Must be called only once upon setting 'readable' listener
      read: onReadFn,
      highWaterMark: 0,
    });

    let pushedNull = false;
    // This will trigger read(0) but must only be called after push(null)
    // because the we haven't pushed any data
    const onReadableFn = mock.fn(() => {
      strictEqual(r.read(), null);
      strictEqual(pushedNull, true);
    });
    r.on('readable', onReadableFn);
    r.on('end', resolve);
    queueMicrotask(() => {
      strictEqual(r.read(), null);
      pushedNull = true;
      r.push(null);
    });

    await promise;
    strictEqual(onReadFn.mock.callCount(), 1);
    strictEqual(onReadableFn.mock.callCount(), 1);
  },
};
