import {
  deepEqual,
  deepStrictEqual,
  doesNotMatch,
  doesNotReject,
  doesNotThrow,
  equal,
  fail,
  ifError,
  match,
  notDeepEqual,
  notDeepStrictEqual,
  notEqual,
  notStrictEqual,
  ok,
  rejects,
  strictEqual,
  throws,
} from 'node:assert';

import {
  Buffer,
} from 'node:buffer';

import {
  Readable,
  Writable,
  Duplex,
  Stream,
  Transform,
  PassThrough,
  addAbortSignal,
  destroy,
  finished,
  isDisturbed,
  isErrored,
  pipeline,
  promises,
} from 'node:stream';

import {
  ByteLengthQueuingStrategy,
} from 'node:stream/web';
strictEqual(ByteLengthQueuingStrategy, globalThis.ByteLengthQueuingStrategy);

import {
  EventEmitter,
} from 'node:events';

function deferredPromise() {
  let resolve, reject;
  const promise = new Promise((a, b) => {
    resolve = a;
    reject = b;
  });
  return {
    promise,
    resolve,
    reject,
  };
}

function countedDeferred(n, action) {
  let resolve;
  const promise = new Promise((a) => {
    resolve = a;
  });
  return {
    promise,
    resolve(...args) {
      action(...args);
      if (--n === 0) resolve();
    }
  };
}

export const legacyAliases = {
  async test(ctrl, env, ctx) {
    strictEqual(Readable, (await import('node:_stream_readable')).Readable);
    strictEqual(Writable, (await import('node:_stream_writable')).Writable);
    strictEqual(Duplex, (await import('node:_stream_duplex')).Duplex);
    strictEqual(Transform, (await import('node:_stream_transform')).Transform);
    strictEqual(PassThrough, (await import('node:_stream_passthrough')).PassThrough);
  }
};

export const addAbortSignalTest = {
  test(ctrl, env, ctx) {
    const ac = new AbortController();
    addAbortSignal(ac.signal, new Readable());

    {
      throws(() => {
        addAbortSignal('INVALID_SIGNAL');
      }, /ERR_INVALID_ARG_TYPE/);

      throws(() => {
        addAbortSignal(ac.signal, 'INVALID_STREAM');
      }, /ERR_INVALID_ARG_TYPE/);
    }
  }
};

export const autoDestroy = {
  async test(ctrl, env, ctx) {
    {
      const rDestroyed = deferredPromise();
      const rEnded = deferredPromise();
      const rClosed = deferredPromise();

      const r = new Readable({
        autoDestroy: true,
        read() {
          this.push('hello');
          this.push('world');
          this.push(null);
        },
        destroy(err, cb) {
          rDestroyed.resolve();
          cb();
        }
      });

      let ended = false;
      r.resume();
      r.on('end', rEnded.resolve);
      r.on('close', rClosed.resolve);

      await Promise.all([
        rDestroyed.promise,
        rEnded.promise,
        rClosed.promise,
      ]);
    }

    {
      const rDestroyed = deferredPromise();
      const rFinished = deferredPromise();
      const rClosed = deferredPromise();

      const w = new Writable({
        autoDestroy: true,
        write(data, enc, cb) {
          cb(null);
        },
        destroy(err, cb) {
          rDestroyed.resolve();
          cb();
        }
      });

      w.write('hello');
      w.write('world');
      w.end();

      w.on('finish', rFinished.resolve);
      w.on('close', rClosed.resolve);

      await Promise.all([
        rDestroyed.promise,
        rFinished.promise,
        rClosed.promise,
      ]);
    }

    {
      const rDestroyed = deferredPromise();
      const rEnded = deferredPromise();
      const rFinished = deferredPromise();
      const rClosed = deferredPromise();

      const t = new Transform({
        autoDestroy: true,
        transform(data, enc, cb) {
          cb(null, data);
        },
        destroy(err, cb) {
          rDestroyed.resolve();
          cb();
        }
      });

      t.write('hello');
      t.write('world');
      t.end();

      t.resume();

      t.on('end', rEnded.resolve);
      t.on('finish', rFinished.resolve);
      t.on('close', rClosed.resolve);

      await Promise.all([
        rDestroyed.promise,
        rEnded.promise,
        rFinished.promise,
        rClosed.promise,
      ]);
    }

    {
      const rDestroyed = deferredPromise();

      const err = new Error('fail');

      const r = new Readable({
        read() {
          r2.emit('error', err);
        }
      });
      const r2 = new Readable({
        autoDestroy: true,
        destroy(err, cb) {
          rDestroyed.resolve(err);
        }
      });

      r.pipe(r2);

      const check = await rDestroyed.promise;
      strictEqual(check, err);
    }

    {
      const rDestroyed = deferredPromise();
      const err = new Error('fail');
      const r = new Readable({
        read() {
          w.emit('error', err);
        }
      });

      const w = new Writable({
        autoDestroy: true,
        destroy(err, cb) {
          rDestroyed.resolve(err);
          cb();
        }
      });

      r.pipe(w);

      const check = await rDestroyed.promise;
      strictEqual(check, err);
    }
  }
};

export const awaitDrainWritersInSyncRecursionWrite = {
  async test(ctrl, env, ctx) {

    const encode = new PassThrough({
      highWaterMark: 1
    });

    const decode = new PassThrough({
      highWaterMark: 1
    });

    const send = countedDeferred(4, (buf) => {
      encode.write(buf);
    });

    let i = 0;
    const onData = () => {
      if (++i === 2) {
        send.resolve(Buffer.from([0x3]));
        send.resolve(Buffer.from([0x4]));
      }
    };

    encode.pipe(decode).on('data', onData);

    send.resolve(Buffer.from([0x1]));
    send.resolve(Buffer.from([0x2]));

    await send.promise;

  }
};

export const backpressure = {
  async test(ctrl, env, ctx) {
    let pushes = 0;
    const total = 65500 + 40 * 1024;
    let reads = 0;
    let writes = 0;
    const rs = new Readable({
      read() {
        if (++reads > 11) throw new Error('incorrect number of reads');
        if (pushes++ === 10) {
          this.push(null);
          return;
        }

        const length = this._readableState.length;

        // We are at most doing two full runs of _reads
        // before stopping, because Readable is greedy
        // to keep its buffer full
        ok(length <= total);

        this.push(Buffer.alloc(65500));
        for (let i = 0; i < 40; i++) {
          this.push(Buffer.alloc(1024));
        }

        // We will be over highWaterMark at this point
        // but a new call to _read is scheduled anyway.
      }
    });

    const closed = deferredPromise();

    const ws = Writable({
      write(data, enc, cb) {
        queueMicrotask(cb);
        if (++writes === 410) closed.resolve();
      }
    });

    rs.pipe(ws);

    await closed.promise;
  }
};

export const bigPacket = {
  async test(ctrl, env, ctx) {
    const rPassed = deferredPromise();

    class TestStream extends Transform {
      _transform(chunk, encoding, done) {
        // Char 'a' only exists in the last write
        if (chunk.includes('a'))
          rPassed.resolve();
        done();
      }
    }

    const s1 = new Transform({
      transform(chunk, encoding, cb) {
        queueMicrotask(() => cb(null, chunk));
      }
    });
    const s2 = new PassThrough();
    const s3 = new TestStream();
    s1.pipe(s3);
    // Don't let s2 auto close which may close s3
    s2.pipe(s3, { end: false });

    // We must write a buffer larger than highWaterMark
    const big = Buffer.alloc(s1.writableHighWaterMark + 1, 'x');

    // Since big is larger than highWaterMark, it will be buffered internally.
    ok(!s1.write(big));
    // 'tiny' is small enough to pass through internal buffer.
    ok(s2.write('tiny'));

    // Write some small data in next IO loop, which will never be written to s3
    // Because 'drain' event is not emitted from s1 and s1 is still paused
    queueMicrotask(() => s1.write('later'));

    await rPassed.promise;
  }
};

export const bigPush = {
  async test(ctrl, env, ctx) {
    const str = 'asdfasdfasdfasdfasdf';

    const r = new Readable({
      highWaterMark: 5,
      encoding: 'utf8'
    });

    let reads = 0;

    function _read() {
      if (reads === 0) {
        setTimeout(() => {
          r.push(str);
        }, 1);
        reads++;
      } else if (reads === 1) {
        const ret = r.push(str);
        strictEqual(ret, false);
        reads++;
      } else {
        r.push(null);
      }
    }

    const read = countedDeferred(3, _read);
    const ended = deferredPromise();

    r._read = read.resolve;
    r.on('end', ended.resolve);

    // Push some data in to start.
    // We've never gotten any read event at this point.
    const ret = r.push(str);
    // Should be false.  > hwm
    ok(!ret);
    let chunk = r.read();
    strictEqual(chunk, str);
    chunk = r.read();
    strictEqual(chunk, null);

    r.once('readable', () => {
      // This time, we'll get *all* the remaining data, because
      // it's been added synchronously, as the read WOULD take
      // us below the hwm, and so it triggered a _read() again,
      // which synchronously added more, which we then return.
      chunk = r.read();
      strictEqual(chunk, str + str);

      chunk = r.read();
      strictEqual(chunk, null);
    });

    await Promise.all([
      read.promise,
      ended.promise,
    ]);
  }
};

export const catchRejections = {
  async test(ctrl, env, ctx) {
    {
      const rErrored = deferredPromise();

      const r = new Readable({
        captureRejections: true,
        read() {
        }
      });
      r.push('hello');
      r.push('world');

      const err = new Error('kaboom');

      r.on('error', (_err) => {
        strictEqual(err, _err);
        strictEqual(r.destroyed, true);
        rErrored.resolve();
      });

      r.on('data', async () => {
        throw err;
      });

      await rErrored.promise;
    }

    {
      const wErrored = deferredPromise();

      const w = new Writable({
        captureRejections: true,
        highWaterMark: 1,
        write(chunk, enc, cb) {
          queueMicrotask(cb);
        }
      });

      const err = new Error('kaboom');

      w.write('hello', () => {
        w.write('world');
      });

      w.on('error', (_err) => {
        strictEqual(err, _err);
        strictEqual(w.destroyed, true);
        wErrored.resolve();
      });

      w.on('drain', async () => {
        throw err;
      });

      await wErrored.promise;
    }
  }
};

export const construct = {
  async test(ctrl, env, ctx) {
    {
      const errored = deferredPromise();
      // Multiple callback.
      new Writable({
        construct: (callback) => {
          callback();
          callback();
        }
      }).on('error', (err) => {
        errored.resolve(err);
      });

      const check = await errored.promise;
      strictEqual(check.message, 'Callback called multiple times');
    }

    {
      const errored = deferredPromise();
      // Multiple callback.
      new Readable({
        construct: (callback) => {
          callback();
          callback();
        }
      }).on('error', (err) => {
        errored.resolve(err);
      });

      const check = await errored.promise;
      strictEqual(check.message, 'Callback called multiple times');
    }

    {
      // Synchronous error.
      const errored = deferredPromise();
      const err = new Error('test');
      new Writable({
        construct: (callback) => {
          callback(err);
        }
      }).on('error', (err) => {
        errored.resolve(err);
      });

      const check = await errored.promise;
      strictEqual(check, err);
    }

    {
      const errored = deferredPromise();
      const err = new Error('test');

      new Readable({
        construct: (callback) => {
          callback(err);
        }
      }).on('error', (err) => {
        errored.resolve(err);
      });

      const check = await errored.promise;
      strictEqual(check, err);
    }

    {
      // Asynchronous error.
      const errored = deferredPromise();
      const err = new Error('test');

      new Writable({
        construct: (callback) => {
          queueMicrotask(() => callback(err));
        }
      }).on('error', (err) => {
        errored.resolve(err);
      });

      const check = await errored.promise;
      strictEqual(check, err);
    }

    {
      // Asynchronous error.
      const errored = deferredPromise();
      const err = new Error('test');

      new Readable({
        construct: (callback) => {
          queueMicrotask(() => callback(err));
        }
      }).on('error', errored.resolve);

      const check = await errored.promise;
      strictEqual(check, err);
    }

    async function testDestroy(factory) {
      {
        const constructed = deferredPromise();
        const closed = deferredPromise();
        const s = factory({
          construct: (cb) => {
            constructed.resolve();
            queueMicrotask(cb);
          }
        });
        s.on('close', closed.resolve);
        s.destroy();
        await Promise.all([
          constructed.promise,
          closed.promise,
        ]);
      }

      {
        const constructed = deferredPromise();
        const closed = deferredPromise();
        const destroyed = deferredPromise();
        const s = factory({
          construct: (cb) => {
            constructed.resolve();
            queueMicrotask(cb);
          }
        });
        s.on('close', closed.resolve);
        s.destroy(null, () => {
          destroyed.resolve();
        });

        await Promise.all([
          constructed.promise,
          closed.promise,
          destroyed.promise,
        ]);
      }

      {
        const constructed = deferredPromise();
        const closed = deferredPromise();
        const s = factory({
          construct: (cb) => {
            constructed.resolve();
            queueMicrotask(cb);
          }
        });
        s.on('close', closed.resolve);
        s.destroy();
        await Promise.all([
          constructed.promise,
          closed.promise,
        ]);
      }


      {
        const constructed = deferredPromise();
        const closed = deferredPromise();
        const errored = deferredPromise();

        const s = factory({
          construct: (cb) => {
            constructed.resolve();
            queueMicrotask(cb);
          }
        });
        s.on('close', closed.resolve);
        s.on('error', (err) => {
          strictEqual(err.message, 'kaboom');
          errored.resolve();
        });
        s.destroy(new Error('kaboom'), (err) => {
          strictEqual(err.message, 'kaboom');
        });
        await Promise.all([
          constructed.promise,
          closed.promise,
          errored.promise,
        ]);
      }

      {
        const constructed = deferredPromise();
        const errored = deferredPromise();
        const closed = deferredPromise();
        const s = factory({
          construct: (cb) => {
            constructed.resolve();
            queueMicrotask(cb);
          }
        });
        s.on('error', errored.resolve);
        s.on('close', closed.resolve);
        s.destroy(new Error());

        await Promise.all([
          constructed.promise,
          closed.promise,
          errored.promise,
        ]);
      }
    }
    await testDestroy((opts) => new Readable({
      read: () => { throw new Error("must not call"); },
      ...opts
    }));
    await testDestroy((opts) => new Writable({
      write: () => { throw new Error("must not call"); },
      final: () => { throw new Error("must not call"); },
      ...opts
    }));

    {
      const constructed = deferredPromise();
      const read = deferredPromise();
      const closed = deferredPromise();

      const r = new Readable({
        autoDestroy: true,
        construct: (cb) => {
          constructed.resolve();
          queueMicrotask(cb);
        },
        read: () => {
          r.push(null);
          read.resolve();
        }
      });
      r.on('close', closed.resolve);
      r.on('data', () => { throw new Error('should not call'); });

      await Promise.all([
        constructed.promise,
        read.promise,
        closed.promise,
      ]);
    }

    {
      const constructed = deferredPromise();
      const write = deferredPromise();
      const final = deferredPromise();
      const closed = deferredPromise();

      const w = new Writable({
        autoDestroy: true,
        construct: (cb) => {
          constructed.resolve();
          queueMicrotask(cb);
        },
        write: (chunk, encoding, cb) => {
          write.resolve();
          queueMicrotask(cb);
        },
        final: (cb) => {
          final.resolve();
          queueMicrotask(cb);
        }
      });
      w.on('close', closed.resolve);
      w.end('data');

      await Promise.all([
        constructed.promise,
        write.promise,
        final.promise,
        closed.promise,
      ]);
    }

    {
      const constructed = deferredPromise();
      const final = deferredPromise();
      const closed = deferredPromise();
      const w = new Writable({
        autoDestroy: true,
        construct: (cb) => {
          constructed.resolve();
          queueMicrotask(cb);
        },
        write: () => { throw new Error('should not call'); },
        final: (cb) => {
          final.resolve();
          queueMicrotask(cb);
        }
      });
      w.on('close', closed.resolve);
      w.end();

      await Promise.all([
        constructed.promise,
        final.promise,
        closed.promise,
      ]);
    }

    {
      let constructed = true;
      new Duplex({
        construct() {
          constructed = true;
        }
      });
      ok(constructed);
    }

    {
      // https://github.com/nodejs/node/issues/34448

      const constructed = deferredPromise();
      const closed = deferredPromise();
      const d = new Duplex({
        readable: false,
        construct: (callback) => {
          queueMicrotask(() => {
            constructed.resolve();
            callback();
          });
        },
        write(chunk, encoding, callback) {
          callback();
        },
        read() {
          this.push(null);
        }
      });
      d.resume();
      d.end('foo');
      d.on('close', closed.resolve);

      await Promise.all([
        constructed.promise,
        closed.promise,
      ]);
    }

    {
      // Construct should not cause stream to read.
      new Readable({
        construct: (callback) => {
          callback();
        },
        read() {
          throw new Error('should not call');
        }
      });
    }
  }
};

export const decoderObjectMode = {
  test(ctrl, env, ctx) {
    const readable = new Readable({
      read: () => {},
      encoding: 'utf16le',
      objectMode: true
    });

    readable.push(Buffer.from('abc', 'utf16le'));
    readable.push(Buffer.from('def', 'utf16le'));
    readable.push(null);

    // Without object mode, these would be concatenated into a single chunk.
    strictEqual(readable.read(), 'abc');
    strictEqual(readable.read(), 'def');
    strictEqual(readable.read(), null);
  }
};

export const destroyEventOrder = {
  async test(ctrl, env, ctx) {
    const destroyed = deferredPromise();
    const events = [];
    const rs = new Readable({
      read() {}
    });

    let closed = false;
    let errored = false;

    rs.on('close', () => events.push('close'));

    rs.on('error', (err) => events.push('errored'));

    rs.destroy(new Error('kaboom'), () => {
      strictEqual(events, ['errored', 'close']);
      destroyed.resolve();
    });

    await destroyed;
  }
};

export const destroyTests = {
  async test(ctrl, env, ctx) {
    {
      const errored = deferredPromise();
      const closed = deferredPromise();
      const r = new Readable({ read() {} });
      destroy(r);
      strictEqual(r.destroyed, true);
      r.on('error', errored.resolve);
      r.on('close', closed.resolve);
      const check = await errored.promise;
      strictEqual(check.name, 'AbortError');
      await closed.promise;
    }

    {
      const errored = deferredPromise();
      const closed = deferredPromise();
      const err = new Error('asd');
      const r = new Readable({ read() {} });
      destroy(r, err);
      strictEqual(r.destroyed, true);
      r.on('error', errored.resolve);
      r.on('close', closed.resolve);
      const check = await errored.promise;
      strictEqual(check, err);
      await closed.promise;
    }

    {
      const errored = deferredPromise();
      const closed = deferredPromise();
      const w = new Writable({ write() {} });
      destroy(w);
      strictEqual(w.destroyed, true);
      w.on('error', errored.resolve);
      w.on('close', closed.resolve);
      const check = await errored.promise;
      strictEqual(check.name, 'AbortError');
      await closed.promise;
    }

    {
      const errored = deferredPromise();
      const closed = deferredPromise();
      const err = new Error('asd');
      const w = new Writable({ write() {} });
      destroy(w, err);
      strictEqual(w.destroyed, true);
      w.on('error', errored.resolve);
      w.on('close', closed.resolve);
      const check = await errored.promise;
      strictEqual(check, err);
      await closed.promise;
    }
  }
};

export const duplexDestroy = {
  async test(ctrl, env, ctx) {
    {
      const closed = deferredPromise();

      const duplex = new Duplex({
        write(chunk, enc, cb) { cb(); },
        read() {}
      });

      duplex.resume();

      duplex.on('end', () => { throw new Error('should not call') });
      duplex.on('finish', () => { throw new Error('should not call') });
      duplex.on('close', closed.resolve);

      duplex.destroy();
      strictEqual(duplex.destroyed, true);
      await closed.promise;
    }

    {
      const errored = deferredPromise();
      const closed = deferredPromise();
      const duplex = new Duplex({
        write(chunk, enc, cb) { cb(); },
        read() {}
      });
      duplex.resume();

      const expected = new Error('kaboom');

      duplex.on('end', closed.reject);
      duplex.on('finish', closed.reject);
      duplex.on('close', closed.resolve);
      duplex.on('error', errored.resolve);

      duplex.destroy(expected);
      strictEqual(duplex.destroyed, true);
      const check = await errored.promise;
      strictEqual(check, expected);
      await closed.promise;
    }

    {
      const errored = deferredPromise();
      const destroyCalled = deferredPromise();
      const duplex = new Duplex({
        write(chunk, enc, cb) { cb(); },
        read() {}
      });

      const expected = new Error('kaboom');

      duplex._destroy = function(err, cb) {
        strictEqual(err, expected);
        cb(err);
        destroyCalled.resolve();
      };

      duplex.on('finish', errored.reject);
      duplex.on('error', errored.resolve);

      duplex.destroy(expected);
      strictEqual(duplex.destroyed, true);
      const check = await errored.promise;
      strictEqual(check, expected);
      await destroyCalled.promise;
    }

    {
      const closed = deferredPromise();
      const destroyCalled = deferredPromise();
      const expected = new Error('kaboom');
      const duplex = new Duplex({
        write(chunk, enc, cb) { cb(); },
        read() {},
        destroy: function(err, cb) {
          strictEqual(err, expected);
          cb();
          destroyCalled.resolve();
        }
      });
      duplex.resume();

      duplex.on('end', closed.reject);
      duplex.on('finish', closed.reject);

      // Error is swallowed by the custom _destroy
      duplex.on('error', closed.reject);
      duplex.on('close', closed.resolve);

      duplex.destroy(expected);
      strictEqual(duplex.destroyed, true);
      await Promise.all([
        closed.promise,
        destroyCalled.promise,
      ]);
    }

    {
      const destroyCalled = deferredPromise();
      const duplex = new Duplex({
        write(chunk, enc, cb) { cb(); },
        read() {}
      });

      duplex._destroy = function(err, cb) {
        strictEqual(err, null);
        cb();
        destroyCalled.resolve();
      };

      duplex.destroy();
      strictEqual(duplex.destroyed, true);
      await destroyCalled.promise;
    }

    {
      const destroyCalled = deferredPromise();
      const closed = deferredPromise();
      const duplex = new Duplex({
        write(chunk, enc, cb) { cb(); },
        read() {}
      });
      duplex.resume();

      duplex._destroy = function(err, cb) {
        strictEqual(err, null);
        queueMicrotask(() => {
          this.push(null);
          this.end();
          cb();
          destroyCalled.resolve();
        });
      };

      const fail = closed.reject;

      duplex.on('finish', fail);
      duplex.on('end', fail);

      duplex.destroy();

      duplex.removeListener('end', fail);
      duplex.removeListener('finish', fail);
      duplex.on('end', fail);
      duplex.on('finish', fail);
      duplex.on('close', closed.resolve);
      strictEqual(duplex.destroyed, true);
      await Promise.all([
        closed.promise,
        destroyCalled.promise,
      ]);
    }

    {
      const destroyCalled = deferredPromise();
      const errored = deferredPromise();
      const duplex = new Duplex({
        write(chunk, enc, cb) { cb(); },
        read() {}
      });

      const expected = new Error('kaboom');

      duplex._destroy = function(err, cb) {
        strictEqual(err, null);
        cb(expected);
        destroyCalled.resolve();
      };

      duplex.on('error', errored.resolve);

      duplex.destroy();
      strictEqual(duplex.destroyed, true);
      const check = await errored.promise;
      strictEqual(check, expected);
      await destroyCalled.promise;
    }

    {
      const closed = deferredPromise();
      const duplex = new Duplex({
        write(chunk, enc, cb) { cb(); },
        read() {},
        allowHalfOpen: true
      });
      duplex.resume();

      duplex.on('finish', closed.reject);
      duplex.on('end', closed.reject);
      duplex.on('close', closed.resolve);

      duplex.destroy();
      strictEqual(duplex.destroyed, true);
      await closed.promise;
    }

    {
      const closed = deferredPromise();
      const duplex = new Duplex({
        write(chunk, enc, cb) { cb(); },
        read() {},
        destroy() {
          throw new Error('should not be called');
        },
      });

      duplex.destroyed = true;
      strictEqual(duplex.destroyed, true);

      duplex.destroy();
    }

    {
      function MyDuplex() {
        strictEqual(this.destroyed, false);
        this.destroyed = false;
        Duplex.call(this);
      }

      Object.setPrototypeOf(MyDuplex.prototype, Duplex.prototype);
      Object.setPrototypeOf(MyDuplex, Duplex);

      new MyDuplex();
    }

    {
      const closed = deferredPromise();
      const duplex = new Duplex({
        writable: false,
        autoDestroy: true,
        write(chunk, enc, cb) { cb(); },
        read() {},
      });
      duplex.push(null);
      duplex.resume();
      duplex.on('close', closed.resolve);
      await closed.promise;
    }

    {
      const closed = deferredPromise();
      const duplex = new Duplex({
        readable: false,
        autoDestroy: true,
        write(chunk, enc, cb) { cb(); },
        read() {},
      });
      duplex.end();
      duplex.on('close', closed.resolve);
      await closed.promise;
    }

    {
      const closed = deferredPromise();
      const duplex = new Duplex({
        allowHalfOpen: false,
        autoDestroy: true,
        write(chunk, enc, cb) { cb(); },
        read() {},
      });
      duplex.push(null);
      duplex.resume();
      const orgEnd = duplex.end;
      duplex.end = closed.reject;
      duplex.on('end', () => {
        // Ensure end() is called in next tick to allow
        // any pending writes to be invoked first.
        queueMicrotask(() => {
          duplex.end = orgEnd;
        });
      });
      duplex.on('close', closed.resolve);
      await closed.promise;
    }

    {
      // Check abort signal
      const closed = deferredPromise();
      const errored = deferredPromise();
      const controller = new AbortController();
      const { signal } = controller;
      const duplex = new Duplex({
        write(chunk, enc, cb) { cb(); },
        read() {},
        signal,
      });
      let count = 0;
      duplex.on('error', (e) => {
        strictEqual(count++, 0); // Ensure not called twice
        strictEqual(e.name, 'AbortError');
        errored.resolve();
      });
      duplex.on('close', closed.resolve);
      controller.abort();
      await Promise.all([
        errored.promise,
        closed.promise,
      ]);
    }
  }
};

export const duplexEnd = {
  async test(ctrl, env, ctx) {
    {
      const stream = new Duplex({
        read() {}
      });
      strictEqual(stream.allowHalfOpen, true);
      stream.on('finish', () => {
        throw new Error('should not have been called');
      });
      strictEqual(stream.listenerCount('end'), 0);
      stream.resume();
      stream.push(null);
    }

    {
      const finishedCalled = deferredPromise();
      const stream = new Duplex({
        read() {},
        allowHalfOpen: false
      });
      strictEqual(stream.allowHalfOpen, false);
      stream.on('finish', finishedCalled.resolve);
      strictEqual(stream.listenerCount('end'), 0);
      stream.resume();
      stream.push(null);
      await finishedCalled.promise;
    }

    {
      const stream = new Duplex({
        read() {},
        allowHalfOpen: false
      });
      strictEqual(stream.allowHalfOpen, false);
      stream._writableState.ended = true;
      stream.on('finish', () => {
        throw new Error('Should not have been called');
      });
      strictEqual(stream.listenerCount('end'), 0);
      stream.resume();
      stream.push(null);
    }
  }
};

export const duplexProps = {
  test(ctrl, env, ctx) {
    {
      const d = new Duplex({
        objectMode: true,
        highWaterMark: 100
      });

      strictEqual(d.writableObjectMode, true);
      strictEqual(d.writableHighWaterMark, 100);
      strictEqual(d.readableObjectMode, true);
      strictEqual(d.readableHighWaterMark, 100);
    }

    {
      const d = new Duplex({
        readableObjectMode: false,
        readableHighWaterMark: 10,
        writableObjectMode: true,
        writableHighWaterMark: 100
      });

      strictEqual(d.writableObjectMode, true);
      strictEqual(d.writableHighWaterMark, 100);
      strictEqual(d.readableObjectMode, false);
      strictEqual(d.readableHighWaterMark, 10);
    }

  }
};

export const duplexReadableEnd = {
  async test(ctrl, env, ctx) {
    const ended = deferredPromise();
    let loops = 5;

    const src = new Readable({
      read() {
        if (loops--)
          this.push(Buffer.alloc(20000));
      }
    });

    const dst = new Transform({
      transform(chunk, output, fn) {
        this.push(null);
        fn();
      }
    });

    src.pipe(dst);

    dst.on('data', () => { });
    dst.on('end', () => {
      strictEqual(loops, 3);
      ok(src.isPaused());
      ended.resolve();
    });

    await ended.promise;
  }
};

export const duplexReadableWritable = {
  async test(ctrl, env, ctx) {
    {
      const errored = deferredPromise();
      const duplex = new Duplex({
        readable: false
      });
      strictEqual(duplex.readable, false);
      duplex.push('asd');
      duplex.on('error', (err) => {
        strictEqual(err.code, 'ERR_STREAM_PUSH_AFTER_EOF');
        errored.resolve();
      });
      duplex.on('data', errored.reject);
      duplex.on('end', errored.reject);
      await errored.promise;
    }

    {
      const closed = deferredPromise();
      const errored = deferredPromise();
      const duplex = new Duplex({
        writable: false,
        write: closed.reject,
      });
      strictEqual(duplex.writable, false);
      duplex.write('asd');
      duplex.on('error', (err) => {
        strictEqual(err.code, 'ERR_STREAM_WRITE_AFTER_END');
        errored.resolve();
      });
      duplex.on('finish', closed.reject);
      duplex.on('close', closed.resolve);
      await Promise.all([
        closed.promise,
        errored.promise,
      ]);
    }

    {
      const closed = deferredPromise();
      const duplex = new Duplex({
        readable: false
      });
      strictEqual(duplex.readable, false);
      duplex.on('data', closed.reject);
      duplex.on('end', closed.reject);
      duplex.on('close', closed.resolve);
      async function run() {
        for await (const chunk of duplex) {
          ok(false, chunk);
        }
      }
      await Promise.all([
        run(),
        closed.promise,
      ]);
    }
  }
};

export const duplexWritableFinished = {
  async test(ctrl, env, ctx) {
    {
      // Find it on Duplex.prototype
      ok(Object.hasOwn(Duplex.prototype, 'writableFinished'));
    }

    // event
    {
      const finishedCalled = deferredPromise();
      const ended = deferredPromise();
      const duplex = new Duplex();

      duplex._write = (chunk, encoding, cb) => {
        // The state finished should start in false.
        strictEqual(duplex.writableFinished, false);
        cb();
      };

      duplex.on('finish', () => {
        strictEqual(duplex.writableFinished, true);
        finishedCalled.resolve();
      });

      duplex.end('testing finished state', () => {
        strictEqual(duplex.writableFinished, true);
        ended.resolve();
      });

      await Promise.all([
        finishedCalled.promise,
        ended.promise,
      ]);
    }
  }
}

export const duplex = {
  async test(ctrl, env, ctx) {
    const stream = new Duplex({ objectMode: true });

    ok(Duplex() instanceof Duplex);
    ok(stream._readableState.objectMode);
    ok(stream._writableState.objectMode);
    ok(stream.allowHalfOpen);
    strictEqual(stream.listenerCount('end'), 0);

    let written;
    let read;

    stream._write = (obj, _, cb) => {
      written = obj;
      cb();
    };

    stream._read = () => {};

    stream.on('data', (obj) => {
      read = obj;
    });

    stream.push({ val: 1 });
    stream.end({ val: 2 });

    // We have no equivalent for on('exit') ... should we?
    // process.on('exit', () => {
    //   assert.strictEqual(read.val, 1);
    //   assert.strictEqual(written.val, 2);
    // });

    // Duplex.fromWeb
    {
      const dataToRead = Buffer.from('hello');
      const dataToWrite = Buffer.from('world');

      const readable = new ReadableStream({
        start(controller) {
          controller.enqueue(dataToRead);
        },
      });

      const writable = new WritableStream({
        write: (chunk) => {
          strictEqual(chunk, dataToWrite);
        }
      });

      const pair = { readable, writable };
      const duplex = Duplex.fromWeb(pair);

      duplex.write(dataToWrite);
      const p = deferredPromise();
      duplex.once('data', (chunk) => {
        strictEqual(chunk, dataToRead);
        p.resolve();
      });
      await p.promise;
    }

    // Duplex.fromWeb - using utf8 and objectMode
    {
      const dataToRead = 'hello';
      const dataToWrite = 'world';

      const readable = new ReadableStream({
        start(controller) {
          controller.enqueue(dataToRead);
        },
      });

      const writable = new WritableStream({
        write: (chunk) => {
          strictEqual(chunk, dataToWrite);
        }
      });

      const pair = {
        readable,
        writable
      };
      const duplex = Duplex.fromWeb(pair, { encoding: 'utf8', objectMode: true });

      duplex.write(dataToWrite);
      const p = deferredPromise();
      duplex.once('data', (chunk) => {
        strictEqual(chunk, dataToRead);
        p.resolve();
      });
      await p.promise;
    }

    // Duplex.toWeb
    {
      const dataToRead = Buffer.from('hello');
      const dataToWrite = Buffer.from('world');

      const duplex = Duplex({
        read() {
          this.push(dataToRead);
          this.push(null);
        },
        write: (chunk) => {
          strictEqual(chunk, dataToWrite);
        }
      });

      const { writable, readable } = Duplex.toWeb(duplex);
      writable.getWriter().write(dataToWrite);

      const p = deferredPromise();
      readable.getReader().read().then((result) => {
        deepStrictEqual(Buffer.from(result.value), dataToRead);
        p.resolve();
      });
      await p.promise;
    }
  }
};

export const end_of_streams = {
  async test(ctrl, env, ctx) {
    throws(
      () => {
        // Passing empty object to mock invalid stream
        // should throw error
        finished({}, () => {});
      },
      { code: 'ERR_INVALID_ARG_TYPE' }
    );

    const streamObj = new Duplex();
    streamObj.end();
    // Below code should not throw any errors as the
    // streamObj is `Stream`
    finished(streamObj, () => {});
  }
};

export const end_paused = {
  async test(ctrl, env, ctx) {
    const calledRead = deferredPromise();
    const ended = deferredPromise();
    const stream = new Readable();
    stream._read = function() {
      this.push(null);
      calledRead.resolve();
    };

    stream.on('data', ended.reject);
    stream.on('end', ended.resolve);
    stream.pause();

    setTimeout(function() {
      stream.resume();
    });

    await Promise.all([
      calledRead.promise,
      ended.promise,
    ]);
  }
};

export const error_once = {
  async test(ctrl, env, ctx) {
    {
      const errored = deferredPromise();
      const writable = new Writable();
      writable.on('error', errored.resolve);
      writable.end();
      writable.write('h');
      writable.write('h');
      await errored.promise;
    }

    {
      const errored = deferredPromise();
      const readable = new Readable();
      readable.on('error', errored.resolve);
      readable.push(null);
      readable.push('h');
      readable.push('h');
      await errored.promise;
    }

  }
};

export const finishedTest = {
  async test(ctrl, env, ctx) {
    const promisify = (await import('node:util')).promisify;
    {
      const finishedOk = deferredPromise();
      const rs = new Readable({
        read() {}
      });

      finished(rs, (err) => {
        if (err == null) finishedOk.resolve();
        else finishedOk.reject(err);
      });

      rs.push(null);
      rs.resume();

      await finishedOk.promise;
    }

    {
      const finishedOk = deferredPromise();

      const ws = new Writable({
        write(data, enc, cb) {
          cb();
        }
      });

      finished(ws, (err) => {
        if (err == null) finishedOk.resolve();
        else finishedOk.reject(err);
      });

      ws.end();

      await finishedOk.promise;
    }

    {
      const finishedOk = deferredPromise();
      const tr = new Transform({
        transform(data, enc, cb) {
          cb();
        }
      });

      let finish = false;
      let ended = false;

      tr.on('end', () => {
        ended = true;
      });

      tr.on('finish', () => {
        finish = true;
      });

      finished(tr, (err) => {
        ok(finish);
        ok(ended);
        if (err == null) finishedOk.resolve();
        else finishedOk.reject(err);
      });

      tr.end();
      tr.resume();

      await finishedOk.promise;
    }

    {
      const finishedCalled = deferredPromise();
      const rs = new Readable({
        read() {
          this.push(enc.encode("a"));
          this.push(enc.encode("b"));
          this.push(null);
        }
      });

      rs.resume();
      finished(rs, finishedCalled.resolve);
      await finishedCalled.promise;
    }

    {
      const finishedPromise = promisify(finished);

      async function run() {
        const enc = new TextEncoder();
        const rs = new Readable({
          read() {
            this.push(enc.encode("a"));
            this.push(enc.encode("b"));
            this.push(null);
          }
        });

        let ended = false;
        rs.resume();
        rs.on('end', () => {
          ended = true;
        });
        await finishedPromise(rs);
        ok(ended);
      }

      await run();
    }

    {
      // Check pre-cancelled
      const signal = new EventTarget();
      signal.aborted = true;

      const rs = Readable.from((function* () {})());
      const errored = deferredPromise();
      finished(rs, { signal }, errored.resolve);
      const check = await errored.promise;
      strictEqual(check.name, 'AbortError');
    }

    {
      // Check cancelled before the stream ends sync.
      const ac = new AbortController();
      const { signal } = ac;

      const errored = deferredPromise();
      const rs = Readable.from((function* () {})());
      finished(rs, { signal }, errored.resolve);
      ac.abort();
      const check = await errored.promise;
      strictEqual(check.name, 'AbortError');
    }

    {
      // Check cancelled before the stream ends async.
      const ac = new AbortController();
      const { signal } = ac;

      const rs = Readable.from((function* () {})());
      setTimeout(() => ac.abort(), 1);
      const errored = deferredPromise();
      finished(rs, { signal }, errored.resolve);
      const check = await errored.promise;
      strictEqual(check.name, 'AbortError');
    }

    {
      // Check cancelled after doesn't throw.
      const ac = new AbortController();
      const { signal } = ac;

      const rs = Readable.from((function* () {
        yield 5;
        queueMicrotask(() => ac.abort());
      })());
      rs.resume();
      const finishedOk = deferredPromise();
      finished(rs, { signal }, finishedOk.resolve);
      const check = await finishedOk.promise;
      strictEqual(check.name, 'AbortError');
    }

    {
      // Promisified abort works
      const finishedPromise = promisify(finished);
      async function run() {
        const ac = new AbortController();
        const { signal } = ac;
        const rs = Readable.from((function* () {})());
        queueMicrotask(() => ac.abort());
        await finishedPromise(rs, { signal });
      }

      await rejects(run, { name: 'AbortError' });
    }

    {
      // Promisified pre-aborted works
      const finishedPromise = promisify(finished);
      async function run() {
        const signal = new EventTarget();
        signal.aborted = true;
        const rs = Readable.from((function* () {})());
        await finishedPromise(rs, { signal });
      }

      await rejects(run, { name: 'AbortError' });
    }

    {
      const rs = new Readable();

      const finishedOk = deferredPromise();

      finished(rs, finishedOk.resolve);

      rs.push(null);
      rs.emit('close'); // Should not trigger an error
      rs.resume();

      const check = await finishedOk.promise;
      ifError(check);
    }

    {
      const rs = new Readable();
      const errored = deferredPromise();

      finished(rs, errored.resolve);

      rs.emit('close'); // Should trigger error
      rs.push(null);
      rs.resume();

      const check = await errored.promise;
      ok(check);
    }

    // Test faulty input values and options.
    {
      const rs = new Readable({
        read() {}
      });

      throws(
        () => finished(rs, 'foo'),
        {
          code: 'ERR_INVALID_ARG_TYPE',
          message: /callback/
        }
      );
      throws(
        () => finished(rs, 'foo', () => {}),
        {
          code: 'ERR_INVALID_ARG_TYPE',
          message: /options/
        }
      );
      throws(
        () => finished(rs, {}, 'foo'),
        {
          code: 'ERR_INVALID_ARG_TYPE',
          message: /callback/
        }
      );

      const finishedOk = deferredPromise();
      finished(rs, null, finishedOk.resolve);

      rs.push(null);
      rs.resume();

      const check = await finishedOk.promise;
      ifError(check);
    }

    // Test that calling returned function removes listeners
    {
      const ws = new Writable({
        write(data, env, cb) {
          cb();
        }
      });
      const removeListener = finished(ws, () => {
        throw new Error('should not have been called');
      });
      removeListener();
      ws.end();
    }

    {
      const rs = new Readable();
      const removeListeners = finished(rs, () => {
        throw new Error('should not have been called');
      });
      removeListeners();

      rs.emit('close');
      rs.push(null);
      rs.resume();
    }

    {
      const EventEmitter = (await import("node:events")).EventEmitter;
      const streamLike = new EventEmitter();
      streamLike.readableEnded = true;
      streamLike.readable = true;
      throws(
        () => {
          finished(streamLike, () => {});
        },
        { code: 'ERR_INVALID_ARG_TYPE' }
      );
      streamLike.emit('close');
    }

    {
      const writable = new Writable({ write() {} });
      writable.writable = false;
      writable.destroy();
      const errored = deferredPromise();
      finished(writable, errored.resolve);
      const check = await errored.promise;
      strictEqual(check.code, 'ERR_STREAM_PREMATURE_CLOSE');
    }

    {
      const readable = new Readable();
      readable.readable = false;
      readable.destroy();
      const errored = deferredPromise();
      finished(readable, errored.resolve);
      const check = await errored.promise;
      strictEqual(check.code, 'ERR_STREAM_PREMATURE_CLOSE');
    }
  }
};

export const highWaterMark = {
  async test(ctrl, env, ctx) {
    {
      // This test ensures that the stream implementation correctly handles values
      // for highWaterMark which exceed the range of signed 32 bit integers and
      // rejects invalid values.

      // This number exceeds the range of 32 bit integer arithmetic but should still
      // be handled correctly.
      const ovfl = Number.MAX_SAFE_INTEGER
      const readable = Readable({
        highWaterMark: ovfl
      })
      strictEqual(readable._readableState.highWaterMark, ovfl)
      const writable = Writable({
        highWaterMark: ovfl
      })
      strictEqual(writable._writableState.highWaterMark, ovfl)
      for (const invalidHwm of [true, false, '5', {}, -5, NaN]) {
        for (const type of [Readable, Writable]) {
          throws(
            () => {
              type({
                highWaterMark: invalidHwm
              })
            },
            {
              name: 'TypeError',
              code: 'ERR_INVALID_ARG_VALUE',
            }
          )
        }
      }
    }
    {
      // This test ensures that the push method's implementation
      // correctly handles the edge case where the highWaterMark and
      // the state.length are both zero

      const readable = Readable({
        highWaterMark: 0
      })
      for (let i = 0; i < 3; i++) {
        const needMoreData = readable.push()
        strictEqual(needMoreData, true)
      }
    }
    {
      // This test ensures that the read(n) method's implementation
      // correctly handles the edge case where the highWaterMark, state.length
      // and n are all zero

      const readable = Readable({
        highWaterMark: 0
      })
      const readCalled = deferredPromise();
      readable._read = readCalled.resolve;
      readable.read(0)
      await readCalled.promise;
    }
    {
      const readCalled = deferredPromise();
      // Parse size as decimal integer
      await Promise.all(['1', '1.0', 1].map(async (size) => {
        const readable = new Readable({
          read: readCalled.resolve,
          highWaterMark: 0
        })
        readable.read(size)
        strictEqual(readable._readableState.highWaterMark, Number(size))
        await readCalled.promise;
      }));
    }
    {
      // Test highwatermark limit
      const hwm = 0x40000000 + 1
      const readable = Readable({
        read() {}
      })
      throws(
        () => readable.read(hwm),
        {
          code: 'ERR_OUT_OF_RANGE',
        }
      )
    }
  }
};

export const pauseThenRead = {
  async test(ctrl, env, ctx) {
    const totalChunks = 100;
    const chunkSize = 99;
    const expectTotalData = totalChunks * chunkSize;
    let expectEndingData = expectTotalData;
    const closed = deferredPromise();
    const r = new Readable({
      highWaterMark: 1000
    })
    r.on('close', closed.resolve);
    let chunks = totalChunks;
    r._read = function (n) {
      if (!(chunks % 2)) queueMicrotask(push);
      else if (!(chunks % 3)) queueMicrotask(push);
      else push();
    }
    let totalPushed = 0;
    function push() {
      const chunk = chunks-- > 0 ? Buffer.alloc(chunkSize, 'x') : null;
      if (chunk) {
        totalPushed += chunk.length;
      }
      r.push(chunk);
    }
    read100();
    await closed.promise;

    // First we read 100 bytes.
    function read100() {
      readn(100, onData);
    }
    function readn(n, then) {
      expectEndingData -= n;
      ;(function read() {
        const c = r.read(n);
        if (!c) r.once('readable', read);
        else {
          strictEqual(c.length, n);
          ok(!r.readableFlowing);
          then();
        }
      })();
    }

    // Then we listen to some data events.
    function onData() {
      expectEndingData -= 100;
      let seen = 0;
      r.on('data', function od(c) {
        seen += c.length;
        if (seen >= 100) {
          // Seen enough
          r.removeListener('data', od);
          r.pause();
          if (seen > 100) {
            // Oh no, seen too much!
            // Put the extra back.
            const diff = seen - 100;
            r.unshift(c.slice(c.length - diff));
          }

          // Nothing should be lost in-between.
          queueMicrotask(pipeLittle);
        }
      });
    }

    // Just pipe 200 bytes, then unshift the extra and unpipe.
    function pipeLittle() {
      expectEndingData -= 200;
      const w = new Writable();
      let written = 0;
      w.on('finish', () => {
        strictEqual(written, 200);
        queueMicrotask(read1234);
      });
      w._write = function (chunk, encoding, cb) {
        written += chunk.length;
        if (written >= 200) {
          r.unpipe(w);
          w.end();
          cb();
          if (written > 200) {
            const diff = written - 200;
            written -= diff;
            r.unshift(chunk.slice(chunk.length - diff));
          }
        } else {
          queueMicrotask(cb);
        }
      }
      r.pipe(w);
    }

    // Now read 1234 more bytes.
    function read1234() {
      readn(1234, resumePause);
    }
    function resumePause() {
      // Don't read anything, just resume and re-pause a whole bunch.
      r.resume();
      r.pause();
      r.resume();
      r.pause();
      r.resume();
      r.pause();
      r.resume();
      r.pause();
      r.resume();
      r.pause();
      queueMicrotask(pipe);
    }
    function pipe() {
      const w = new Writable();
      let written = 0;
      w._write = function (chunk, encoding, cb) {
        written += chunk.length;
        cb();
      };
      w.on('finish', () => {
        strictEqual(written, expectEndingData);
        strictEqual(totalPushed, expectTotalData);
      });
      r.pipe(w);
    }
  }
};

export const corkUncork = {
  async test(ctrl, env, ctx) {
    // Test the buffering behavior of Writable streams.
    //
    // The call to cork() triggers storing chunks which are flushed
    // on calling uncork() in the same tick.
    //
    // node version target: 0.12

    const closed = deferredPromise();
    const expectedChunks = ['please', 'buffer', 'me', 'kindly'];
    const inputChunks = expectedChunks.slice(0);
    let seenChunks = [];
    let seenEnd = false;
    const w = new Writable();
    // Let's arrange to store the chunks.
    w._write = function (chunk, encoding, cb) {
      // Default encoding given none was specified.
      strictEqual(encoding, 'buffer');
      seenChunks.push(chunk);
      cb();
    };
    // Let's record the stream end event.
    w.on('finish', () => {
      seenEnd = true;
    });

    w.on('close', closed.resolve);

    function writeChunks(remainingChunks, callback) {
      const writeChunk = remainingChunks.shift();
      let writeState;
      if (writeChunk) {
        queueMicrotask(() => {
          writeState = w.write(writeChunk);
          // We were not told to stop writing.
          ok(writeState);
          writeChunks(remainingChunks, callback);
        })
      } else {
        callback();
      }
    }

    // Do an initial write.
    w.write('stuff');
    // The write was immediate.
    strictEqual(seenChunks.length, 1);
    // Reset the chunks seen so far.
    seenChunks = [];

    // Trigger stream buffering.
    w.cork();

    // Write the bufferedChunks.
    writeChunks(inputChunks, () => {
      // Should not have seen anything yet.
      strictEqual(seenChunks.length, 0);

      // Trigger writing out the buffer.
      w.uncork();

      // Buffered bytes should be seen in current tick.
      strictEqual(seenChunks.length, 4);

      // Did the chunks match.
      for (let i = 0, l = expectedChunks.length; i < l; i++) {
        const seen = seenChunks[i];
        // There was a chunk.
        ok(seen);
        const expected = Buffer.from(expectedChunks[i]);
        // It was what we expected.
        ok(seen.equals(expected));
      }
      queueMicrotask(() => {
        // The stream should not have been ended.
        ok(!seenEnd);
        w.end();
      });
    });

    await closed.promise;
  }
};

export const corkEnd = {
  async test(ctrl, env, ctx) {
    // Test the buffering behavior of Writable streams.
    //
    // The call to cork() triggers storing chunks which are flushed
    // on calling end() and the stream subsequently ended.
    //
    // node version target: 0.12

    const closed = deferredPromise();
    const expectedChunks = ['please', 'buffer', 'me', 'kindly'];
    const inputChunks = expectedChunks.slice(0);
    let seenChunks = [];
    let seenEnd = false;
    const w = new Writable();
    // Let's arrange to store the chunks.
    w._write = function (chunk, encoding, cb) {
      // Stream end event is not seen before the last write.
      ok(!seenEnd);
      // Default encoding given none was specified.
      strictEqual(encoding, 'buffer');
      seenChunks.push(chunk);
      cb();
    };
    // Let's record the stream end event.
    w.on('finish', () => {
      seenEnd = true;
    });
    function writeChunks(remainingChunks, callback) {
      const writeChunk = remainingChunks.shift();
      let writeState;
      if (writeChunk) {
        queueMicrotask(() => {
          writeState = w.write(writeChunk);
          // We were not told to stop writing.
          ok(writeState);
          writeChunks(remainingChunks, callback);
        });
      } else {
        callback();
      }
    }

    w.on('close', closed.resolve);

    // Do an initial write.
    w.write('stuff');
    // The write was immediate.
    strictEqual(seenChunks.length, 1);
    // Reset the seen chunks.
    seenChunks = [];

    // Trigger stream buffering.
    w.cork();

    // Write the bufferedChunks.
    writeChunks(inputChunks, () => {
      // Should not have seen anything yet.
      strictEqual(seenChunks.length, 0);

      // Trigger flush and ending the stream.
      w.end();

      // Stream should not ended in current tick.
      ok(!seenEnd);

      // Buffered bytes should be seen in current tick.
      strictEqual(seenChunks.length, 4);

      // Did the chunks match.
      for (let i = 0, l = expectedChunks.length; i < l; i++) {
        const seen = seenChunks[i];
        // There was a chunk.
        ok(seen);
        const expected = Buffer.from(expectedChunks[i]);
        // It was what we expected.
        ok(seen.equals(expected));
      }
      queueMicrotask(() => {
        // Stream should have ended in next tick.
        ok(seenEnd);
      });
    });

    await closed.promise;
  }
};

export const writable2Writable = {
  async test(ctrl, env, ctx) {
    class TestWriter extends Writable {
      constructor(opts) {
        super(opts);
        this.buffer = [];
        this.written = 0;
      }
      _write(chunk, encoding, cb) {
        // Simulate a small unpredictable latency
        queueMicrotask(() => {
          this.buffer.push(chunk.toString());
          this.written += chunk.length;
          cb();
        }, Math.floor(Math.random() * 10));
      }
    }
    const chunks = new Array(50);
    for (let i = 0; i < chunks.length; i++) {
      chunks[i] = 'x'.repeat(i);
    }
    {
      // Verify fast writing
      const tw = new TestWriter({
        highWaterMark: 100
      });
      const finishCalled = deferredPromise();
      tw.on(
        'finish',
        function () {
          // Got chunks in the right order
          deepStrictEqual(tw.buffer, chunks);
          finishCalled.resolve();
        }
      );
      chunks.forEach(function (chunk) {
        // Ignore backpressure. Just buffer it all up.
        tw.write(chunk);
      });
      tw.end();
      await finishCalled.promise;
    }
    {
      // Verify slow writing
      const tw = new TestWriter({
        highWaterMark: 100
      });
      const finishedCalled = deferredPromise();
      tw.on(
        'finish',
        function () {
          //  Got chunks in the right order
          deepStrictEqual(tw.buffer, chunks);
          finishedCalled.resolve();
        }
      );
      let i = 0;
      (function W() {
        tw.write(chunks[i++]);
        if (i < chunks.length) setTimeout(W, 10);
        else tw.end();
      })();
      await finishedCalled.promise;
    }
    {
      // Verify write backpressure
      const tw = new TestWriter({
        highWaterMark: 50
      });
      let drains = 0;
      const finishCalled = deferredPromise();
      tw.on(
        'finish',
        function () {
          // Got chunks in the right order
          deepStrictEqual(tw.buffer, chunks);
          strictEqual(drains, 17);
          finishCalled.resolve();
        }
      );
      tw.on('drain', function () {
        drains++
      });
      let i = 0;
      (function W() {
        let ret;
        do {
          ret = tw.write(chunks[i++]);
        } while (ret !== false && i < chunks.length)
        if (i < chunks.length) {
          ok(tw.writableLength >= 50);
          tw.once('drain', W);
        } else {
          tw.end();
        }
      })();
      await finishCalled.promise;
    }
    {
      // Verify write callbacks
      const callbacks = chunks
        .map(function (chunk, i) {
          return [
            i,
            function () {
              callbacks._called[i] = chunk
            }
          ];
        })
        .reduce(function (set, x) {
          set[`callback-${x[0]}`] = x[1];
          return set;
        }, {});
      callbacks._called = [];
      const finishCalled = deferredPromise();
      const tw = new TestWriter({
        highWaterMark: 100
      });
      tw.on(
        'finish',
        function () {
          queueMicrotask(
            function () {
              // Got chunks in the right order
              deepStrictEqual(tw.buffer, chunks);
              // Called all callbacks
              deepStrictEqual(callbacks._called, chunks);
              finishCalled.resolve();
            }
          );
        }
      );
      chunks.forEach(function (chunk, i) {
        tw.write(chunk, callbacks[`callback-${i}`]);
      });
      tw.end();
      await finishCalled.promise;
    }
    {
      // Verify end() callback
      const tw = new TestWriter();
      const endCalled = deferredPromise();
      tw.end(endCalled.resolve);
      await endCalled.promise;
    }
    const helloWorldBuffer = Buffer.from('hello world');
    {
      // Verify end() callback with chunk
      const tw = new TestWriter();
      const endCalled = deferredPromise();
      tw.end(helloWorldBuffer, endCalled.resolve);
      await endCalled.promise;
    }
    {
      // Verify end() callback with chunk and encoding
      const tw = new TestWriter()
      const endCalled = deferredPromise();
      tw.end('hello world', 'ascii', endCalled.resolve);
      await endCalled.promise;
    }
    {
      // Verify end() callback after write() call
      const tw = new TestWriter();
      const endCalled = deferredPromise();
      tw.write(helloWorldBuffer);
      tw.end(endCalled.resolve);
      await endCalled.promise;
    }
    {
      // Verify end() callback after write() callback
      const tw = new TestWriter();
      const endCalled = deferredPromise();
      let writeCalledback = false;
      tw.write(helloWorldBuffer, function () {
        writeCalledback = true;
      });
      tw.end(function () {
        strictEqual(writeCalledback, true);
        endCalled.resolve();
      });
      await endCalled.promise;
    }
    {
      // Verify encoding is ignored for buffers
      const tw = new Writable();
      const hex = '018b5e9a8f6236ffe30e31baf80d2cf6eb';
      const writeCalled = deferredPromise();
      tw._write = function (chunk) {
        strictEqual(chunk.toString('hex'), hex);
        writeCalled.resolve();
      };
      const buf = Buffer.from(hex, 'hex');
      tw.write(buf, 'latin1');
      await writeCalled.promise;
    }
    {
      // Verify writables cannot be piped
      const w = new Writable({
        autoDestroy: false
      })
      const gotError = deferredPromise();
      w._write = gotError.reject;
      w.on('error', gotError.resolve)
      w.pipe(new Writable({write() {}}));
      await gotError.promise;
    }
    {
      // Verify that duplex streams can be piped
      const d = new Duplex();
      const readCalled = deferredPromise;
      d._read = readCalled.resolve;
      d._write = () => { throw new Error('should not be called'); };
      let gotError = false;
      d.on('error', function () {
        gotError = true;
      });
      d.pipe(new Writable({ write() {}}));
      strictEqual(gotError, false);
    }
    {
      // Verify that end(chunk) twice is an error
      const w = new Writable();
      const writeCalled = deferredPromise();
      const gotError = deferredPromise();
      w._write = (msg) => {
        strictEqual(msg.toString(), 'this is the end');
        writeCalled.resolve();
      };
      w.on('error', function (er) {
        strictEqual(er.message, 'write after end');
        gotError.resolve();
      });
      w.end('this is the end');
      w.end('and so is this');
      await Promise.all([
        writeCalled.promise,
        gotError.promise,
      ]);
    }
    {
      // Verify stream doesn't end while writing
      const w = new Writable();
      const wrote = deferredPromise();
      const finishCalled = deferredPromise();
      w._write = function (chunk, e, cb) {
        strictEqual(this.writing, undefined);
        wrote.resolve();
        this.writing = true;
        queueMicrotask(() => {
          this.writing = false;
          cb();
        }, 1);
      }
      w.on(
        'finish',
        function () {
          strictEqual(this.writing, false);
          finishCalled.resolve();
        }
      );
      w.write(Buffer.alloc(0));
      w.end();
      await Promise.all([
        wrote.promise,
        finishCalled.promise,
      ]);
    }
    {
      // Verify finish does not come before write() callback
      const w = new Writable();
      const finishCalled = deferredPromise();
      let writeCb = false;
      w._write = function (chunk, e, cb) {
        setTimeout(function () {
          writeCb = true;
          cb();
        }, 10);
      };
      w.on(
        'finish',
        function () {
          strictEqual(writeCb, true);
          finishCalled.resolve();
        }
      )
      w.write(Buffer.alloc(0));
      w.end();
      await finishCalled.promise;
    }
    {
      // Verify finish does not come before synchronous _write() callback
      const w = new Writable();
      const finishCalled = deferredPromise();
      let writeCb = false;
      w._write = function (chunk, e, cb) {
        cb();
      };
      w.on(
        'finish',
        function () {
          strictEqual(writeCb, true);
          finishCalled.resolve();
        }
      );
      w.write(Buffer.alloc(0), function () {
        writeCb = true;
      });
      w.end();
      await finishCalled.promise;
    }
    {
      // Verify finish is emitted if the last chunk is empty
      const w = new Writable();
      w._write = function (chunk, e, cb) {
        queueMicrotask(cb);
      };
      const finishCalled = deferredPromise();
      w.on('finish', finishCalled.resolve);
      w.write(Buffer.allocUnsafe(1));
      w.end(Buffer.alloc(0));
      await finishCalled.promise;
    }
    {
      // Verify that finish is emitted after shutdown
      const w = new Writable();
      let shutdown = false;
      const finalCalled = deferredPromise();
      const finishCalled = deferredPromise();
      w._final = function (cb) {
        strictEqual(this, w);
        setTimeout(function () {
          finalCalled.resolve();
          shutdown = true;
          cb();
        }, 100);
      };
      w._write = function (chunk, e, cb) {
        queueMicrotask(cb);
      };
      w.on(
        'finish',
        function () {
          strictEqual(shutdown, true);
          finishCalled.resolve();
        }
      );
      w.write(Buffer.allocUnsafe(1));
      w.end(Buffer.allocUnsafe(0));
      await Promise.all([
        finalCalled.promise,
        finishCalled.promise,
      ]);
    }
    {
      // Verify that error is only emitted once when failing in _finish.
      const w = new Writable();
      const finalCalled = deferredPromise();
      const gotError = deferredPromise();
      w._final = function (cb) {
        cb(new Error('test'));
        finalCalled.resolve();
      };
      w.on(
        'error',
        (err) => {
          gotError.resolve();
          strictEqual(w._writableState.errorEmitted, true);
          strictEqual(err.message, 'test');
          w.on('error', () => { throw new Error('should not be called again'); });
          w.destroy(new Error());
        }
      );
      w.end();
      await Promise.all([
        finalCalled.promise,
        gotError.promise,
      ]);
    }
    {
      // Verify that error is only emitted once when failing in write.
      const w = new Writable();
      throws(
        () => {
          w.write(null);
        },
        {
          code: 'ERR_STREAM_NULL_VALUES'
        }
      );
    }
    {
      // Verify that error is only emitted once when failing in write after end.
      const w = new Writable();
      const gotError = deferredPromise();
      w.on(
        'error',
        (err) => {
          strictEqual(w._writableState.errorEmitted, true);
          strictEqual(err.code, 'ERR_STREAM_WRITE_AFTER_END');
          gotError.resolve();
        }
      );
      w.end();
      w.write('hello');
      w.destroy(new Error());
      await gotError.promise;
    }
    {
      // Verify that finish is not emitted after error
      const w = new Writable();
      const finalCalled = deferredPromise();
      const gotError = deferredPromise();
      w._final = function (cb) {
        cb(new Error());
        finalCalled.resolve();
      };
      w._write = function (chunk, e, cb) {
        queueMicrotask(cb);
      };
      w.on('error', gotError.resolve);
      w.on('prefinish', gotError.reject);
      w.on('finish', gotError.reject);
      w.write(Buffer.allocUnsafe(1));
      w.end(Buffer.allocUnsafe(0));
      await Promise.all([
        finalCalled.promise,
        gotError.promise,
      ]);
    }
  }
};

export const unpipeLeak = {
  async test(ctrl, env, ctx) {
    const chunk = Buffer.from('hallo')
    class TestWriter extends Writable {
      _write(buffer, encoding, callback) {
        callback(null);
      }
    }
    const dest = new TestWriter();

    // Set this high so that we'd trigger a nextTick warning
    // and/or RangeError if we do maybeReadMore wrong.
    class TestReader extends Readable {
      constructor() {
        super({
          highWaterMark: 0x10000
        });
      }
      _read(size) {
        this.push(chunk);
      }
    }
    const src = new TestReader();
    for (let i = 0; i < 10; i++) {
      src.pipe(dest);
      src.unpipe(dest);
    }

    strictEqual(src.listeners('end').length, 0)
    strictEqual(src.listeners('readable').length, 0)
    strictEqual(dest.listeners('unpipe').length, 0)
    strictEqual(dest.listeners('drain').length, 0)
    strictEqual(dest.listeners('error').length, 0)
    strictEqual(dest.listeners('close').length, 0)
    strictEqual(dest.listeners('finish').length, 0)
  }
};

export const unpipeDrain = {
  async test(ctrl, env, ctx) {
    class TestWriter extends Writable {
      _write(buffer, encoding, callback) {}
    }

    const dest = new TestWriter();
    class TestReader extends Readable {
      done = deferredPromise();
      constructor() {
        super();
        this.reads = 0;
      }
      _read(size) {
        this.reads += 1;
        this.push(Buffer.alloc(size));
        if (this.reads == 2) this.done.resolve();
      }
    }
    const src1 = new TestReader();
    const src2 = new TestReader();
    src1.pipe(dest);
    src1.once('readable', () => {
      queueMicrotask(() => {
        src2.pipe(dest);
        src2.once('readable', () => {
          queueMicrotask(() => {
            src1.unpipe(dest);
          });
        });
      });
    });
    await Promise.all([
      src1.done.promise,
      src2.done.promise,
    ]);
  }
};

export const transformTest = {
  async test(ctrl, env, ctx) {
    {
      // Verify writable side consumption
      const tx = new Transform({
        highWaterMark: 10
      });
      let transformed = 0;
      tx._transform = function (chunk, encoding, cb) {
        transformed += chunk.length;
        tx.push(chunk);
        cb();
      };
      for (let i = 1; i <= 10; i++) {
        tx.write(Buffer.allocUnsafe(i));
      }
      tx.end();
      strictEqual(tx.readableLength, 10);
      strictEqual(transformed, 10);
      deepStrictEqual(
        tx.writableBuffer.map(function (c) {
          return c.chunk.length;
        }),
        [5, 6, 7, 8, 9, 10]
      );
    }
    {
      // Verify passthrough behavior
      const pt = new PassThrough();
      pt.write(Buffer.from('foog'));
      pt.write(Buffer.from('bark'));
      pt.write(Buffer.from('bazy'));
      pt.write(Buffer.from('kuel'));
      pt.end();
      strictEqual(pt.read(5).toString(), 'foogb');
      strictEqual(pt.read(5).toString(), 'arkba');
      strictEqual(pt.read(5).toString(), 'zykue');
      strictEqual(pt.read(5).toString(), 'l');
    }
    {
      // Verify object passthrough behavior
      const pt = new PassThrough({
        objectMode: true
      });
      pt.write(1);
      pt.write(true);
      pt.write(false);
      pt.write(0);
      pt.write('foo');
      pt.write('');
      pt.write({
        a: 'b'
      });
      pt.end();
      strictEqual(pt.read(), 1);
      strictEqual(pt.read(), true);
      strictEqual(pt.read(), false);
      strictEqual(pt.read(), 0);
      strictEqual(pt.read(), 'foo');
      strictEqual(pt.read(), '');
      deepStrictEqual(pt.read(), {
        a: 'b'
      });
    }
    {
      // Verify passthrough constructor behavior
      const pt = PassThrough();
      ok(pt instanceof PassThrough);
    }
    {
      // Verify transform constructor behavior
      const pt = Transform();
      ok(pt instanceof Transform);
    }
    {
      // Perform a simple transform
      const pt = new Transform();
      pt._transform = function (c, e, cb) {
        const ret = Buffer.alloc(c.length, 'x');
        pt.push(ret);
        cb();
      }
      pt.write(Buffer.from('foog'));
      pt.write(Buffer.from('bark'));
      pt.write(Buffer.from('bazy'));
      pt.write(Buffer.from('kuel'));
      pt.end();
      strictEqual(pt.read(5).toString(), 'xxxxx');
      strictEqual(pt.read(5).toString(), 'xxxxx');
      strictEqual(pt.read(5).toString(), 'xxxxx');
      strictEqual(pt.read(5).toString(), 'x');
    }
    {
      // Verify simple object transform
      const pt = new Transform({
        objectMode: true
      });
      pt._transform = function (c, e, cb) {
        pt.push(JSON.stringify(c));
        cb();
      };
      pt.write(1);
      pt.write(true);
      pt.write(false);
      pt.write(0);
      pt.write('foo');
      pt.write('');
      pt.write({
        a: 'b'
      });
      pt.end();
      strictEqual(pt.read(), '1');
      strictEqual(pt.read(), 'true');
      strictEqual(pt.read(), 'false');
      strictEqual(pt.read(), '0');
      strictEqual(pt.read(), '"foo"');
      strictEqual(pt.read(), '""');
      strictEqual(pt.read(), '{"a":"b"}');
    }
    {
      // Verify async passthrough
      const pt = new Transform();
      pt._transform = function (chunk, encoding, cb) {
        setTimeout(function () {
          pt.push(chunk);
          cb();
        }, 10);
      }
      pt.write(Buffer.from('foog'));
      pt.write(Buffer.from('bark'));
      pt.write(Buffer.from('bazy'));
      pt.write(Buffer.from('kuel'));
      pt.end();
      const finishCalled = deferredPromise();
      pt.on(
        'finish',
        function () {
          strictEqual(pt.read(5).toString(), 'foogb');
          strictEqual(pt.read(5).toString(), 'arkba');
          strictEqual(pt.read(5).toString(), 'zykue');
          strictEqual(pt.read(5).toString(), 'l');
          finishCalled.resolve();
        }
      )
      await finishCalled.promise;
    }
    {
      // Verify asymmetric transform (expand)
      const pt = new Transform();

      // Emit each chunk 2 times.
      pt._transform = function (chunk, encoding, cb) {
        setTimeout(function () {
          pt.push(chunk);
          setTimeout(function () {
            pt.push(chunk);
            cb();
          }, 10);
        }, 10);
      }
      pt.write(Buffer.from('foog'));
      pt.write(Buffer.from('bark'));
      pt.write(Buffer.from('bazy'));
      pt.write(Buffer.from('kuel'));
      pt.end();
      const finishCalled = deferredPromise();
      pt.on(
        'finish',
        function () {
          strictEqual(pt.read(5).toString(), 'foogf');
          strictEqual(pt.read(5).toString(), 'oogba');
          strictEqual(pt.read(5).toString(), 'rkbar');
          strictEqual(pt.read(5).toString(), 'kbazy');
          strictEqual(pt.read(5).toString(), 'bazyk');
          strictEqual(pt.read(5).toString(), 'uelku');
          strictEqual(pt.read(5).toString(), 'el');
          finishCalled.resolve();
        }
      );
      await finishCalled.promise;
    }
    {
      // Verify asymmetric transform (compress)
      const pt = new Transform();

      // Each output is the first char of 3 consecutive chunks,
      // or whatever's left.
      pt.state = '';
      pt._transform = function (chunk, encoding, cb) {
        if (!chunk) chunk = '';
        const s = chunk.toString();
        setTimeout(() => {
          this.state += s.charAt(0);
          if (this.state.length === 3) {
            pt.push(Buffer.from(this.state));
            this.state = '';
          }
          cb();
        }, 10);
      }
      pt._flush = function (cb) {
        // Just output whatever we have.
        pt.push(Buffer.from(this.state));
        this.state = '';
        cb();
      }
      pt.write(Buffer.from('aaaa'));
      pt.write(Buffer.from('bbbb'));
      pt.write(Buffer.from('cccc'));
      pt.write(Buffer.from('dddd'));
      pt.write(Buffer.from('eeee'));
      pt.write(Buffer.from('aaaa'));
      pt.write(Buffer.from('bbbb'));
      pt.write(Buffer.from('cccc'));
      pt.write(Buffer.from('dddd'));
      pt.write(Buffer.from('eeee'));
      pt.write(Buffer.from('aaaa'));
      pt.write(Buffer.from('bbbb'));
      pt.write(Buffer.from('cccc'));
      pt.write(Buffer.from('dddd'));
      pt.end();

      const finishCalled = deferredPromise();
      // 'abcdeabcdeabcd'
      pt.on(
        'finish',
        function () {
          strictEqual(pt.read(5).toString(), 'abcde');
          strictEqual(pt.read(5).toString(), 'abcde');
          strictEqual(pt.read(5).toString(), 'abcd');
          finishCalled.resolve();
        }
      )
      await finishCalled.promise;
    }

    // This tests for a stall when data is written to a full stream
    // that has empty transforms.
    {
      // Verify complex transform behavior
      let count = 0;
      let saved = null;
      const pt = new Transform({
        highWaterMark: 3
      });
      pt._transform = function (c, e, cb) {
        if (count++ === 1) saved = c;
        else {
          if (saved) {
            pt.push(saved);
            saved = null;
          }
          pt.push(c);
        }
        cb();
      };
      const finishCalled = deferredPromise();
      pt.on('finish', finishCalled.resolve);
      pt.once('readable', function () {
        queueMicrotask(function () {
          pt.write(Buffer.from('d'));
          pt.write(
            Buffer.from('ef'),
            function () {
              pt.end();
            }
          );
          strictEqual(pt.read().toString(), 'abcdef');
          strictEqual(pt.read(), null);
        });
      });
      pt.write(Buffer.from('abc'));
      await finishCalled.promise;
    }
    {
      // Verify passthrough event emission
      const pt = new PassThrough();
      let emits = 0;
      pt.on('readable', function () {
        emits++;
      });
      pt.write(Buffer.from('foog'));
      pt.write(Buffer.from('bark'));
      strictEqual(emits, 0);
      strictEqual(pt.read(5).toString(), 'foogb');
      strictEqual(String(pt.read(5)), 'null');
      strictEqual(emits, 0);
      pt.write(Buffer.from('bazy'));
      pt.write(Buffer.from('kuel'));
      strictEqual(emits, 0);
      strictEqual(pt.read(5).toString(), 'arkba');
      strictEqual(pt.read(5).toString(), 'zykue');
      strictEqual(pt.read(5), null);
      pt.end();
      strictEqual(emits, 1);
      strictEqual(pt.read(5).toString(), 'l');
      strictEqual(pt.read(5), null);
      strictEqual(emits, 1);
    }
    {
      // Verify passthrough event emission reordering
      const pt = new PassThrough();
      let emits = 0;
      pt.on('readable', function () {
        emits++;
      });
      pt.write(Buffer.from('foog'));
      pt.write(Buffer.from('bark'));
      strictEqual(emits, 0);
      strictEqual(pt.read(5).toString(), 'foogb');
      strictEqual(pt.read(5), null);
      const readable1 = deferredPromise();
      const readable2 = deferredPromise();
      const readable3 = deferredPromise();
      pt.once(
        'readable',
        function () {
          strictEqual(pt.read(5).toString(), 'arkba');
          strictEqual(pt.read(5), null);
          pt.once(
            'readable',
            function () {
              strictEqual(pt.read(5).toString(), 'zykue');
              strictEqual(pt.read(5), null);
              pt.once(
                'readable',
                function () {
                  strictEqual(pt.read(5).toString(), 'l');
                  strictEqual(pt.read(5), null);
                  strictEqual(emits, 3);
                  readable3.resolve();
                }
              )
              pt.end();
              readable2.resolve();
            }
          )
          pt.write(Buffer.from('kuel'));
          readable1.resolve();
        }
      )
      pt.write(Buffer.from('bazy'));
      await Promise.all([
        readable1.promise,
        readable2.promise,
        readable3.promise,
      ]);
    }
    {
      // Verify passthrough facade
      const pt = new PassThrough();
      const datas = [];
      pt.on('data', function (chunk) {
        datas.push(chunk.toString());
      });
      const endCalled = deferredPromise();
      pt.on(
        'end',
        function () {
          deepStrictEqual(datas, ['foog', 'bark', 'bazy', 'kuel']);
          endCalled.resolve();
        }
      )
      pt.write(Buffer.from('foog'));
      setTimeout(function () {
        pt.write(Buffer.from('bark'));
        setTimeout(function () {
          pt.write(Buffer.from('bazy'));
          setTimeout(function () {
            pt.write(Buffer.from('kuel'));
            setTimeout(function () {
              pt.end();
            }, 10);
          }, 10);
        }, 10);
      }, 10);

      await endCalled.promise;
    }
    {
      // Verify object transform (JSON parse)
      const jp = new Transform({
        objectMode: true
      });
      jp._transform = function (data, encoding, cb) {
        try {
          jp.push(JSON.parse(data));
          cb();
        } catch (er) {
          cb(er);
        }
      };

      // Anything except null/undefined is fine.
      // those are "magic" in the stream API, because they signal EOF.
      const objects = [
        {
          foo: 'bar'
        },
        100,
        'string',
        {
          nested: {
            things: [
              {
                foo: 'bar'
              },
              100,
              'string'
            ]
          }
        }
      ]
      const ended = deferredPromise();
      jp.on('end', ended.resolve);
      objects.forEach(function (obj) {
        jp.write(JSON.stringify(obj));
        const res = jp.read();
        deepStrictEqual(res, obj);
      })
      jp.end();
      // Read one more time to get the 'end' event
      jp.read();
      await ended.promise;
    }
    {
      // Verify object transform (JSON stringify)
      const js = new Transform({
        objectMode: true
      });
      js._transform = function (data, encoding, cb) {
        try {
          js.push(JSON.stringify(data));
          cb();
        } catch (er) {
          cb(er);
        }
      };

      // Anything except null/undefined is fine.
      // those are "magic" in the stream API, because they signal EOF.
      const objects = [
        {
          foo: 'bar'
        },
        100,
        'string',
        {
          nested: {
            things: [
              {
                foo: 'bar'
              },
              100,
              'string'
            ]
          }
        }
      ];
      const ended = deferredPromise();
      js.on('end', ended.resolve);
      objects.forEach(function (obj) {
        js.write(obj);
        const res = js.read();
        strictEqual(res, JSON.stringify(obj));
      })
      js.end();
      // Read one more time to get the 'end' event
      js.read();
      await ended.promise;
    }
  }
};

export const set_encoding = {
  async test(ctrl, env, ctx) {
    class TestReader extends Readable {
      constructor(n, opts) {
        super(opts);
        this.pos = 0;
        this.len = n || 100;
      }
      _read(n) {
        setTimeout(() => {
          if (this.pos >= this.len) {
            // Double push(null) to test eos handling
            this.push(null);
            return this.push(null);
          }
          n = Math.min(n, this.len - this.pos);
          if (n <= 0) {
            // Double push(null) to test eos handling
            this.push(null);
            return this.push(null);
          }
          this.pos += n;
          const ret = Buffer.alloc(n, 'a');
          return this.push(ret);
        }, 1)
      }
    }
    {
      // Verify utf8 encoding
      const tr = new TestReader(100);
      tr.setEncoding('utf8');
      const out = [];
      const expect = [
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa'
      ];
      tr.on('readable', function flow() {
        let chunk;
        while (null !== (chunk = tr.read(10))) out.push(chunk);
      });
      const ended = deferredPromise();
      tr.on(
        'end',
        function () {
          deepStrictEqual(out, expect)
          ended.resolve();
        }
      );
      await ended.promise;
    }
    {
      // Verify hex encoding
      const tr = new TestReader(100);
      tr.setEncoding('hex');
      const out = [];
      const expect = [
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161'
      ];
      tr.on('readable', function flow() {
        let chunk;
        while (null !== (chunk = tr.read(10))) out.push(chunk);
      });
      const ended = deferredPromise();
      tr.on(
        'end',
        function () {
          deepStrictEqual(out, expect);
          ended.resolve();
        }
      )
      await ended.promise;
    }
    {
      // Verify hex encoding with read(13)
      const tr = new TestReader(100);
      tr.setEncoding('hex');
      const out = [];
      const expect = [
        '6161616161616',
        '1616161616161',
        '6161616161616',
        '1616161616161',
        '6161616161616',
        '1616161616161',
        '6161616161616',
        '1616161616161',
        '6161616161616',
        '1616161616161',
        '6161616161616',
        '1616161616161',
        '6161616161616',
        '1616161616161',
        '6161616161616',
        '16161'
      ];
      tr.on('readable', function flow() {
        let chunk;
        while (null !== (chunk = tr.read(13))) out.push(chunk);
      })
      const ended = deferredPromise();
      tr.on(
        'end',
        function () {
          deepStrictEqual(out, expect);
          ended.resolve();
        }
      );
      await ended.promise;
    }
    {
      // Verify base64 encoding
      const tr = new TestReader(100);
      tr.setEncoding('base64');
      const out = [];
      const expect = [
        'YWFhYWFhYW',
        'FhYWFhYWFh',
        'YWFhYWFhYW',
        'FhYWFhYWFh',
        'YWFhYWFhYW',
        'FhYWFhYWFh',
        'YWFhYWFhYW',
        'FhYWFhYWFh',
        'YWFhYWFhYW',
        'FhYWFhYWFh',
        'YWFhYWFhYW',
        'FhYWFhYWFh',
        'YWFhYWFhYW',
        'FhYQ=='
      ];
      tr.on('readable', function flow() {
        let chunk;
        while (null !== (chunk = tr.read(10))) out.push(chunk);
      })
      const ended = deferredPromise();
      tr.on(
        'end',
        function () {
          deepStrictEqual(out, expect);
          ended.resolve();
        }
      );
      await ended.promise;
    }
    {
      // Verify utf8 encoding
      const tr = new TestReader(100, {
        encoding: 'utf8'
      });
      const out = [];
      const expect = [
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa',
        'aaaaaaaaaa'
      ];
      tr.on('readable', function flow() {
        let chunk;
        while (null !== (chunk = tr.read(10))) out.push(chunk);
      })
      const ended = deferredPromise();
      tr.on(
        'end',
        function () {
          deepStrictEqual(out, expect);
          ended.resolve();
        }
      );
      await ended.promise;
    }
    {
      // Verify hex encoding
      const tr = new TestReader(100, {
        encoding: 'hex'
      });
      const out = [];
      const expect = [
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161',
        '6161616161'
      ];
      tr.on('readable', function flow() {
        let chunk;
        while (null !== (chunk = tr.read(10))) out.push(chunk);
      });
      const ended = deferredPromise();
      tr.on(
        'end',
        function () {
          deepStrictEqual(out, expect);
          ended.resolve();
        }
      );
      await ended.promise;
    }
    {
      // Verify hex encoding with read(13)
      const tr = new TestReader(100, {
        encoding: 'hex'
      });
      const out = [];
      const expect = [
        '6161616161616',
        '1616161616161',
        '6161616161616',
        '1616161616161',
        '6161616161616',
        '1616161616161',
        '6161616161616',
        '1616161616161',
        '6161616161616',
        '1616161616161',
        '6161616161616',
        '1616161616161',
        '6161616161616',
        '1616161616161',
        '6161616161616',
        '16161'
      ];
      tr.on('readable', function flow() {
        let chunk;
        while (null !== (chunk = tr.read(13))) out.push(chunk);
      })
      const ended = deferredPromise();
      tr.on(
        'end',
        function () {
          deepStrictEqual(out, expect);
          ended.resolve();
        }
      );
      await ended.promise;
    }
    {
      // Verify base64 encoding
      const tr = new TestReader(100, {
        encoding: 'base64'
      });
      const out = [];
      const expect = [
        'YWFhYWFhYW',
        'FhYWFhYWFh',
        'YWFhYWFhYW',
        'FhYWFhYWFh',
        'YWFhYWFhYW',
        'FhYWFhYWFh',
        'YWFhYWFhYW',
        'FhYWFhYWFh',
        'YWFhYWFhYW',
        'FhYWFhYWFh',
        'YWFhYWFhYW',
        'FhYWFhYWFh',
        'YWFhYWFhYW',
        'FhYQ=='
      ];
      tr.on('readable', function flow() {
        let chunk;
        while (null !== (chunk = tr.read(10))) out.push(chunk);
      });
      const ended = deferredPromise();
      tr.on(
        'end',
        function () {
          deepStrictEqual(out, expect);
          ended.resolve();
        }
      );
      await ended.promise;
    }
    {
      // Verify chaining behavior
      const tr = new TestReader(100);
      deepStrictEqual(tr.setEncoding('utf8'), tr);
    }
  }
};

export const readable_wrap = {
  async test(ctrl, env, ctx) {
    async function runTest(highWaterMark, objectMode, produce) {
      const rEnded = deferredPromise();
      const old = new EventEmitter();
      const r = new Readable({
        highWaterMark,
        objectMode
      });
      strictEqual(r, r.wrap(old));

      r.on('end', rEnded.resolve);
      old.pause = function () {
        old.emit('pause');
        flowing = false;
      };
      old.resume = function () {
        old.emit('resume');
        flow();
      };

      // Make sure pause is only emitted once.
      let pausing = false
      r.on('pause', () => {
        strictEqual(pausing, false);
        pausing = true;
        queueMicrotask(() => {
          pausing = false;
        })
      });
      let flowing;
      let chunks = 10;
      let oldEnded = false;
      const expected = [];
      function flow() {
        flowing = true;
        while (flowing && chunks-- > 0) {
          const item = produce();
          expected.push(item);
          old.emit('data', item);
        }
        if (chunks <= 0) {
          oldEnded = true;
          old.emit('end');
        }
      }
      const w = new Writable({
        highWaterMark: highWaterMark * 2,
        objectMode
      });
      const written = [];
      w._write = function (chunk, encoding, cb) {
        written.push(chunk)
        setTimeout(cb, 1)
      };
      const finishCalled = deferredPromise();
      w.on(
        'finish',
        function () {
          performAsserts()
          finishCalled.resolve();
        }
      );
      r.pipe(w);
      flow();
      function performAsserts() {
        ok(oldEnded);
        deepStrictEqual(written, expected);
      }
      await Promise.all([
        rEnded.promise,
        finishCalled.promise,
      ]);
    }
    await runTest(100, false, function () {
      return Buffer.allocUnsafe(100);
    })
    await runTest(10, false, function () {
      return Buffer.from('xxxxxxxxxx');
    })
    await runTest(1, true, function () {
      return {
        foo: 'bar'
      };
    })
    const objectChunks = [
      5,
      'a',
      false,
      0,
      '',
      'xyz',
      {
        x: 4
      },
      7,
      [],
      555
    ];
    await runTest(1, true, function () {
      return objectChunks.shift();
    });
  }
};

export const readable_wrap_error = {
  async test(ctrl, env, ctx) {
    class LegacyStream extends EventEmitter {
      pause() {}
      resume() {}
    }
    {
      const err = new Error();
      const oldStream = new LegacyStream();
      const errored = deferredPromise();
      const r = new Readable({
        autoDestroy: true
      })
        .wrap(oldStream)
        .on(
          'error',
          () => {
            strictEqual(r._readableState.errorEmitted, true)
            strictEqual(r._readableState.errored, err)
            strictEqual(r.destroyed, true)
            errored.resolve();
          }
        )
      oldStream.emit('error', err);
      await errored.promise;
    }
    {
      const err = new Error();
      const oldStream = new LegacyStream();
      const errored = deferredPromise();
      const r = new Readable({
        autoDestroy: false
      })
        .wrap(oldStream)
        .on(
          'error',
          () => {
            strictEqual(r._readableState.errorEmitted, true)
            strictEqual(r._readableState.errored, err)
            strictEqual(r.destroyed, false)
            errored.resolve();
          }
        )
      oldStream.emit('error', err)
      await errored.promise;
    }
  }
};

export const readable_wrap_empty = {
  async test(ctrl, env, ctx) {
    const oldStream = new EventEmitter();
    oldStream.pause = () => {};
    oldStream.resume = () => {};
    const newStream = new Readable().wrap(oldStream);
    const ended = deferredPromise();
    newStream.on('readable', () => {}).on('end', ended.resolve);
    oldStream.emit('end');
    await ended.promise;
  }
};

export const readable_wrap_destroy = {
  async test(ctrl, env, ctx) {
    const oldStream = new EventEmitter();
    oldStream.pause = () => {};
    oldStream.resume = () => {};
    {
      const destroyCalled = deferredPromise;
      new Readable({
        autoDestroy: false,
        destroy: destroyCalled.resolve
      }).wrap(oldStream);
      oldStream.emit('destroy');
      await destroyCalled.promise;
    }
    {
      const destroyCalled = deferredPromise;
      new Readable({
        autoDestroy: false,
        destroy: destroyCalled.resolve
      }).wrap(oldStream);
      oldStream.emit('close');
      await destroyCalled.promise;
    }
  }
};

export const readable_non_empty_end = {
  async test(ctrl, env, ctx) {
    let len = 0;
    const chunks = new Array(10);
    for (let i = 1; i <= 10; i++) {
      chunks[i - 1] = Buffer.allocUnsafe(i);
      len += i;
    }
    const test = new Readable();
    let n = 0;
    test._read = function (size) {
      const chunk = chunks[n++];
      setTimeout(function () {
        test.push(chunk === undefined ? null : chunk);
      }, 1);
    }
    test.on('end', thrower);
    function thrower() {
      throw new Error('this should not happen!');
    }
    let bytesread = 0;
    const ended = deferredPromise();
    test.on('readable', function () {
      const b = len - bytesread - 1;
      const res = test.read(b);
      if (res) {
        bytesread += res.length;
        setTimeout(next, 1);
      }
      test.read(0);
    })
    test.read(0);
    function next() {
      // Now let's make 'end' happen
      test.removeListener('end', thrower);
      test.on('end', ended.resolve);

      // One to get the last byte
      let r = test.read();
      ok(r);
      strictEqual(r.length, 1);
      r = test.read();
      strictEqual(r, null);
    }
    await ended.promise;
  }
};

export const readable_legacy_drain = {
  async test(ctrl, env, ctx) {
    const r = new Readable();
    const N = 256;
    let reads = 0;
    r._read = function (n) {
      return r.push(++reads === N ? null : Buffer.allocUnsafe(1));
    };
    const ended1 = deferredPromise();
    r.on('end', ended1.resolve);
    const w = new Stream();
    w.writable = true;
    let buffered = 0;
    w.write = function (c) {
      buffered += c.length;
      queueMicrotask(drain);
      return false;
    }
    function drain() {
      ok(buffered <= 3);
      buffered = 0;
      w.emit('drain');
    }
    const ended2 = deferredPromise();
    w.end = ended2.resolve;
    r.pipe(w);
    await Promise.all([
      ended1.promise,
      ended2.promise,
    ]);
  }
};

export const read_sync_stack = {
  async test(ctrl, env, ctx) {
    // This tests synchronous read callbacks and verifies that even if they nest
    // heavily the process handles it without an error

    const r = new Readable();
    const N = 256 * 1024;
    let reads = 0;
    r._read = function (n) {
      const chunk = reads++ === N ? null : Buffer.allocUnsafe(1);
      r.push(chunk);
    }
    r.on('readable', function onReadable() {
      r.read(N * 2);
    })
    const ended = deferredPromise();
    r.on('end', ended.resolve);
    r.read(0);
    await ended.promise;
  }
};

export const stream2_push = {
  async test(ctrl, env, ctx) {
    const stream = new Readable({
      highWaterMark: 16,
      encoding: 'utf8'
    });
    const source = new EventEmitter()
    stream._read = function () {
      readStart();
    };
    const ended = deferredPromise();
    stream.on('end', ended.resolve);
    source.on('data', function (chunk) {
      const ret = stream.push(chunk);
      if (!ret) readStop();
    })
    source.on('end', function () {
      stream.push(null);
    });
    let reading = false;
    function readStart() {
      reading = true;
    }
    function readStop() {
      reading = false;
      queueMicrotask(function () {
        const r = stream.read();
        if (r !== null) writer.write(r);
      });
    }
    const writer = new Writable({
      decodeStrings: false
    });
    const written = [];
    const expectWritten = [
      'asdfgasdfgasdfgasdfg',
      'asdfgasdfgasdfgasdfg',
      'asdfgasdfgasdfgasdfg',
      'asdfgasdfgasdfgasdfg',
      'asdfgasdfgasdfgasdfg',
      'asdfgasdfgasdfgasdfg'
    ];
    writer._write = function (chunk, encoding, cb) {
      written.push(chunk);
      queueMicrotask(cb);
    }
    writer.on('finish', finish);

    // Now emit some chunks.

    const chunk = 'asdfg';
    let set = 0;
    readStart();
    data();
    function data() {
      ok(reading);
      source.emit('data', chunk);
      ok(reading);
      source.emit('data', chunk);
      ok(reading);
      source.emit('data', chunk);
      ok(reading);
      source.emit('data', chunk);
      ok(!reading);
      if (set++ < 5) setTimeout(data, 10);
      else end();
    }
    function finish() {
      deepStrictEqual(written, expectWritten);
    }
    function end() {
      source.emit('end');
      ok(!reading);
      writer.end(stream.read())
    }
    await ended.promise;
  }
};

export const stream2_pipe_error_once_listener = {
  async test(ctrl, env, ctx) {
    class Read extends Readable {
      _read(size) {
        this.push('x');
        this.push(null);
      }
    }
    class Write extends Writable {
      _write(buffer, encoding, cb) {
        this.emit('error', new Error('boom'));
      }
    }
    const read = new Read();
    const write = new Write();
    const errored = deferredPromise();
    write.once('error', errored.resolve);
    read.pipe(write)

    await errored.promise;
  }
};

export const stream2_pipe_error_handling = {
  async test(ctrl, env, ctx) {
    {
      let count = 1000;
      const source = new Readable();
      source._read = function (n) {
        n = Math.min(count, n);
        count -= n;
        source.push(Buffer.allocUnsafe(n));
      };
      let unpipedDest;
      source.unpipe = function (dest) {
        unpipedDest = dest;
        Readable.prototype.unpipe.call(this, dest);
      };
      const dest = new Writable();
      dest._write = function (chunk, encoding, cb) {
        cb();
      };
      source.pipe(dest);
      let gotErr = null;
      dest.on('error', function (err) {
        gotErr = err;
      });
      let unpipedSource;
      dest.on('unpipe', function (src) {
        unpipedSource = src;
      });
      const err = new Error('This stream turned into bacon.');
      dest.emit('error', err);
      strictEqual(gotErr, err);
      strictEqual(unpipedSource, source);
      strictEqual(unpipedDest, dest);
    }
    {
      let count = 1000;
      const source = new Readable();
      source._read = function (n) {
        n = Math.min(count, n);
        count -= n;
        source.push(Buffer.allocUnsafe(n));
      };
      let unpipedDest;
      source.unpipe = function (dest) {
        unpipedDest = dest;
        Readable.prototype.unpipe.call(this, dest);
      };
      const dest = new Writable({
        autoDestroy: false
      });
      dest._write = function (chunk, encoding, cb) {
        cb();
      };
      source.pipe(dest);
      let unpipedSource;
      dest.on('unpipe', function (src) {
        unpipedSource = src;
      });
      const err = new Error('This stream turned into bacon.');
      let gotErr = null;
      try {
        dest.emit('error', err);
      } catch (e) {
        gotErr = e;
      }
      strictEqual(gotErr, err);
      strictEqual(unpipedSource, source);
      strictEqual(unpipedDest, dest);
    }
  }
};

export const stream2_objects = {
  async test(ctrl, env, ctx) {
    function toArray(callback) {
      const stream = new Writable({
        objectMode: true
      });
      const list = [];
      stream.write = function (chunk) {
        list.push(chunk);
      };
      stream.end = function () {
        callback(list);
        this.ended.resolve();
      };
      stream.ended = deferredPromise();
      return stream;
    }
    function fromArray(list) {
      const r = new Readable({
        objectMode: true
      });
      r._read = () => { throw new Error('should not have been called'); };
      list.forEach(function (chunk) {
        r.push(chunk);
      });
      r.push(null);
      return r;
    }
    {
      // Verify that objects can be read from the stream
      const r = fromArray([
        {
          one: '1'
        },
        {
          two: '2'
        }
      ]);
      const v1 = r.read();
      const v2 = r.read();
      const v3 = r.read();
      deepStrictEqual(v1, {
        one: '1'
      });
      deepStrictEqual(v2, {
        two: '2'
      });
      strictEqual(v3, null);
    }
    {
      // Verify that objects can be piped into the stream
      const r = fromArray([
        {
          one: '1'
        },
        {
          two: '2'
        }
      ]);
      const w = toArray(
        function (list) {
          deepStrictEqual(list, [
            {
              one: '1'
            },
            {
              two: '2'
            }
          ])
        }
      );
      r.pipe(w);
      await w.ended.promise;
    }
    {
      // Verify that read(n) is ignored
      const r = fromArray([
        {
          one: '1'
        },
        {
          two: '2'
        }
      ]);
      const value = r.read(2);
      deepStrictEqual(value, {
        one: '1'
      });
    }
    {
      // Verify that objects can be synchronously read
      const r = new Readable({
        objectMode: true
      });
      const list = [
        {
          one: '1'
        },
        {
          two: '2'
        }
      ];
      r._read = function (n) {
        const item = list.shift()
        r.push(item || null)
      };
      const dest = toArray(
        function (list) {
          deepStrictEqual(list, [
            {
              one: '1'
            },
            {
              two: '2'
            }
          ])
        }
      );
      r.pipe(dest);
      await dest.ended.promise;
    }
    {
      // Verify that objects can be asynchronously read
      const r = new Readable({
        objectMode: true
      });
      const list = [
        {
          one: '1'
        },
        {
          two: '2'
        }
      ];
      r._read = function (n) {
        const item = list.shift();
        queueMicrotask(function () {
          r.push(item || null);
        });
      }
      const dest = toArray(
        function (list) {
          deepStrictEqual(list, [
            {
              one: '1'
            },
            {
              two: '2'
            }
          ])
        }
      );
      r.pipe(dest);
      await dest.ended.promise;
    }
    {
      // Verify that strings can be read as objects
      const r = new Readable({
        objectMode: true
      });
      r._read = () => { throw new Error('should not have been called'); };
      const list = ['one', 'two', 'three'];
      list.forEach(function (str) {
        r.push(str);
      });
      r.push(null);
      const dest = toArray(
        function (array) {
          deepStrictEqual(array, list);
        }
      );
      r.pipe(dest);
      await dest.ended.promise;
    }
    {
      // Verify read(0) behavior for object streams
      const r = new Readable({
        objectMode: true
      });
      r.push('foobar');
      r.push(null);
      const dest = toArray(
        function (array) {
          deepStrictEqual(array, ['foobar'])
        }
      );
      r.pipe(dest);
      await dest.ended.promise;
    }
    {
      // Verify the behavior of pushing falsey values
      const r = new Readable({
        objectMode: true
      });
      r.push(false);
      r.push(0);
      r.push('');
      r.push(null);
      const dest = toArray(
        function (array) {
          deepStrictEqual(array, [false, 0, '']);
        }
      );
      r.pipe(dest);
      await dest.ended.promise;
    }
    {
      // Verify high watermark _read() behavior
      const r = new Readable({
        highWaterMark: 6,
        objectMode: true
      });
      let calls = 0;
      const list = ['1', '2', '3', '4', '5', '6', '7', '8'];
      r._read = function (n) {
        calls++;
      };
      list.forEach(function (c) {
        r.push(c);
      });
      const v = r.read();
      strictEqual(calls, 0);
      strictEqual(v, '1');
      const v2 = r.read();
      strictEqual(v2, '2');
      const v3 = r.read();
      strictEqual(v3, '3');
      strictEqual(calls, 1);
    }
    {
      // Verify high watermark push behavior
      const r = new Readable({
        highWaterMark: 6,
        objectMode: true
      });
      r._read = () => { throw new Error("should not have been called"); };
      for (let i = 0; i < 6; i++) {
        const bool = r.push(i);
        strictEqual(bool, i !== 5);
      }
    }
    {
      // Verify that objects can be written to stream
      const w = new Writable({
        objectMode: true
      });
      w._write = function (chunk, encoding, cb) {
        deepStrictEqual(chunk, {
          foo: 'bar'
        });
        cb();
      }
      const finishCalled = deferredPromise();
      w.on('finish', finishCalled.resolve);
      w.write({
        foo: 'bar'
      });
      w.end();
      await finishCalled.promise;
    }
    {
      // Verify that multiple objects can be written to stream
      const w = new Writable({
        objectMode: true
      });
      const list = [];
      w._write = function (chunk, encoding, cb) {
        list.push(chunk);
        cb();
      };
      const finishCalled = deferredPromise();
      w.on(
        'finish',
        function () {
          deepStrictEqual(list, [0, 1, 2, 3, 4]);
          finishCalled.resolve();
        }
      )
      w.write(0);
      w.write(1);
      w.write(2);
      w.write(3);
      w.write(4);
      w.end();
      await finishCalled.promise;
    }
    {
      // Verify that strings can be written as objects
      const w = new Writable({
        objectMode: true
      });
      const list = [];
      w._write = function (chunk, encoding, cb) {
        list.push(chunk);
        queueMicrotask(cb);
      }
      const finishCalled = deferredPromise();
      w.on(
        'finish',
        function () {
          deepStrictEqual(list, ['0', '1', '2', '3', '4']);
          finishCalled.resolve();
        }
      )
      w.write('0');
      w.write('1');
      w.write('2');
      w.write('3');
      w.write('4');
      w.end();
      await finishCalled.promise;
    }
    {
      // Verify that stream buffers finish until callback is called
      const w = new Writable({
        objectMode: true
      });
      let called = false;
      w._write = function (chunk, encoding, cb) {
        strictEqual(chunk, 'foo');
        queueMicrotask(function () {
          called = true;
          cb();
        });
      }
      const finishCalled = deferredPromise();
      w.on(
        'finish',
        function () {
          strictEqual(called, true);
          finishCalled.resolve();
        }
      )
      w.write('foo');
      w.end();
      await finishCalled.promise;
    }
  }
};

export const stream2_large_read_stall = {
  async test(ctrl, env, ctx) {
    // If everything aligns so that you do a read(n) of exactly the
    // remaining buffer, then make sure that 'end' still emits.

    const READSIZE = 100;
    const PUSHSIZE = 20;
    const PUSHCOUNT = 1000;
    const HWM = 50;
    const r = new Readable({
      highWaterMark: HWM
    });
    const rs = r._readableState;
    r._read = push;
    r.on('readable', function () {
      let ret;
      do {
        ret = r.read(READSIZE);
      } while (ret && ret.length === READSIZE);
    });
    const ended = deferredPromise();
    r.on(
      'end',
      function () {
        strictEqual(pushes, PUSHCOUNT + 1);
        ended.resolve();
      }
    )
    let pushes = 0;
    function push() {
      if (pushes > PUSHCOUNT) return;
      if (pushes++ === PUSHCOUNT) {
        return r.push(null);
      }
      if (r.push(Buffer.allocUnsafe(PUSHSIZE))) setTimeout(push, 1);
    }
    await ended.promise;
  }
};

export const stream2_decode_partial = {
  async test(ctrl, env, ctx) {
    let buf = '';
    const euro = Buffer.from([0xe2, 0x82, 0xac]);
    const cent = Buffer.from([0xc2, 0xa2]);
    const source = Buffer.concat([euro, cent]);
    const readable = Readable({
      encoding: 'utf8'
    });
    readable.push(source.slice(0, 2));
    readable.push(source.slice(2, 4));
    readable.push(source.slice(4, 6));
    readable.push(null);
    readable.on('data', function (data) {
      buf += data;
    });
    const closed = deferredPromise();
    readable.on('close', closed.resolve);
    await closed.promise;

    strictEqual(buf, '€¢');
  }
};

export const stream2_compatibility = {
  async test(ctrl, env, ctx) {
    let ondataCalled = 0;
    class TestReader extends Readable {
      constructor() {
        super();
        this._buffer = Buffer.alloc(100, 'x');
        this.on('data', () => {
          ondataCalled++;
        })
      }
      _read(n) {
        this.push(this._buffer);
        this._buffer = Buffer.alloc(0);
      }
    }
    const reader = new TestReader();
    queueMicrotask(function () {
      strictEqual(ondataCalled, 1);
      reader.push(null);
    })
    class TestWriter extends Writable {
      constructor() {
        super();
        this.write('foo');
        this.end();
      }
      _write(chunk, enc, cb) {
        cb();
      }
    }
    const writer = new TestWriter();

    const readerClose = deferredPromise();
    const writerClose = deferredPromise();
    reader.on('close', readerClose.resolve);
    writer.on('close', writerClose.resolve);

    await Promise.all([
      readerClose.promise,
      writerClose.promise,
    ]);

    strictEqual(reader.readable, false);
    strictEqual(writer.writable, false);
  }
};

export const stream2_basic = {
  async test(ctrl, env, ctx) {

    class TestReader extends Readable {
      constructor(n) {
        super()
        this._buffer = Buffer.alloc(n || 100, 'x')
        this._pos = 0
        this._bufs = 10
      }
      _read(n) {
        const max = this._buffer.length - this._pos
        n = Math.max(n, 0)
        const toRead = Math.min(n, max)
        if (toRead === 0) {
          // Simulate the read buffer filling up with some more bytes some time
          // in the future.
          setTimeout(() => {
            this._pos = 0
            this._bufs -= 1
            if (this._bufs <= 0) {
              // read them all!
              if (!this.ended) this.push(null)
            } else {
              // now we have more.
              // kinda cheating by calling _read, but whatever,
              // it's just fake anyway.
              this._read(n)
            }
          }, 10)
          return
        }
        const ret = this._buffer.slice(this._pos, this._pos + toRead)
        this._pos += toRead
        this.push(ret)
      }
    }

    class TestWriter extends EventEmitter {
      constructor() {
        super()
        this.received = []
        this.flush = false
      }
      write(c) {
        this.received.push(c.toString())
        this.emit('write', c)
        return true
      }
      end(c) {
        if (c) this.write(c)
        this.emit('end', this.received)
      }
    }

    {
      // Test basic functionality
      const r = new TestReader(20);
      const reads = [];
      const expect = [
        'x',
        'xx',
        'xxx',
        'xxxx',
        'xxxxx',
        'xxxxxxxxx',
        'xxxxxxxxxx',
        'xxxxxxxxxxxx',
        'xxxxxxxxxxxxx',
        'xxxxxxxxxxxxxxx',
        'xxxxxxxxxxxxxxxxx',
        'xxxxxxxxxxxxxxxxxxx',
        'xxxxxxxxxxxxxxxxxxxxx',
        'xxxxxxxxxxxxxxxxxxxxxxx',
        'xxxxxxxxxxxxxxxxxxxxxxxxx',
        'xxxxxxxxxxxxxxxxxxxxx'
      ];
      r.on(
        'end',
        function () {
          deepStrictEqual(reads, expect);
        }
      );
      let readSize = 1;
      function flow() {
        let res;
        while (null !== (res = r.read(readSize++))) {
          reads.push(res.toString());
        }
        r.once('readable', flow);
      }
      flow();

      await promises.finished(r);
    }
    {
      // Verify pipe
      const r = new TestReader(5);
      const expect = ['xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx'];
      const w = new TestWriter();
      w.on(
        'end',
        function (received) {
          deepStrictEqual(received, expect);
        }
      );
      r.pipe(w);
      await promises.finished(r);
    }
    await Promise.all([1, 2, 3, 4, 5, 6, 7, 8, 9].map(async function (SPLIT) {
      // Verify unpipe
      const r = new TestReader(5);

      // Unpipe after 3 writes, then write to another stream instead.
      let expect = ['xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx'];
      expect = [expect.slice(0, SPLIT), expect.slice(SPLIT)];
      const w = [new TestWriter(), new TestWriter()];
      let writes = SPLIT;
      w[0].on('write', function () {
        if (--writes === 0) {
          r.unpipe();
          deepStrictEqual(r._readableState.pipes, []);
          w[0].end();
          r.pipe(w[1]);
          deepStrictEqual(r._readableState.pipes, [w[1]]);
        }
      });
      let ended = 0;
      w[0].on(
        'end',
        function (results) {
          ended++;
          strictEqual(ended, 1);
          deepStrictEqual(results, expect[0]);
        }
      );
      w[1].on(
        'end',
        function (results) {
          ended++;
          strictEqual(ended, 2);
          deepStrictEqual(results, expect[1]);
        }
      );
      r.pipe(w[0]);
      await promises.finished(r);
    }));
    {
      // Verify both writers get the same data when piping to destinations
      const r = new TestReader(5);
      const w = [new TestWriter(), new TestWriter()];
      const expect = ['xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx'];
      w[0].on(
        'end',
        function (received) {
          deepStrictEqual(received, expect);
        }
      );
      w[1].on(
        'end',
        function (received) {
          deepStrictEqual(received, expect);
        }
      );
      r.pipe(w[0]);
      r.pipe(w[1]);
      await promises.finished(r);
    }
    await Promise.all([1, 2, 3, 4, 5, 6, 7, 8, 9].map(async function (SPLIT) {
      // Verify multi-unpipe
      const r = new TestReader(5);

      // Unpipe after 3 writes, then write to another stream instead.
      let expect = ['xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx', 'xxxxx'];
      expect = [expect.slice(0, SPLIT), expect.slice(SPLIT)];
      const w = [new TestWriter(), new TestWriter(), new TestWriter()];
      let writes = SPLIT;
      w[0].on('write', function () {
        if (--writes === 0) {
          r.unpipe();
          w[0].end();
          r.pipe(w[1]);
        }
      });
      let ended = 0;
      w[0].on(
        'end',
        function (results) {
          ended++
          strictEqual(ended, 1);
          deepStrictEqual(results, expect[0]);
        }
      );
      w[1].on(
        'end',
        function (results) {
          ended++;
          strictEqual(ended, 2);
          deepStrictEqual(results, expect[1]);
        }
      );
      r.pipe(w[0]);
      r.pipe(w[2]);
      await promises.finished(r);
    }));
    {
      // Verify that back pressure is respected
      const r = new Readable({
        objectMode: true
      });
      r._read = () => { throw new Error('should not have been called'); };
      let counter = 0;
      r.push(['one']);
      r.push(['two']);
      r.push(['three']);
      r.push(['four']);
      r.push(null);
      const w1 = new Readable();
      w1.write = function (chunk) {
        strictEqual(chunk[0], 'one');
        w1.emit('close');
        queueMicrotask(function () {
          r.pipe(w2);
          r.pipe(w3);
        });
      };
      w1.end = () => { throw new Error('should not have been called'); };
      r.pipe(w1);
      const expected = ['two', 'two', 'three', 'three', 'four', 'four'];
      const w2 = new Readable();
      w2.write = function (chunk) {
        strictEqual(chunk[0], expected.shift());
        strictEqual(counter, 0);
        counter++;
        if (chunk[0] === 'four') {
          return true;
        }
        setTimeout(function () {
          counter--;
          w2.emit('drain');
        }, 10);
        return false;
      }
      const ended2 = deferredPromise();
      w2.end = ended2.resolve;
      const w3 = new Readable();
      w3.write = function (chunk) {
        strictEqual(chunk[0], expected.shift());
        strictEqual(counter, 1);
        counter++;
        if (chunk[0] === 'four') {
          return true;
        }
        setTimeout(function () {
          counter--;
          w3.emit('drain');
        }, 50);
        return false;
      }
      const ended3 = deferredPromise();
      w3.end = function () {
        strictEqual(counter, 2);
        strictEqual(expected.length, 0);
        ended3.resolve();
      };
      await Promise.all([
        ended2.promise,
        ended3.promise,
      ]);
    }
    {
      // Verify read(0) behavior for ended streams
      const r = new Readable();
      let written = false;
      let ended = false;
      r._read = () => { throw new Error('should not have been called'); };
      r.push(Buffer.from('foo'));
      r.push(null);
      const v = r.read(0);
      strictEqual(v, null);
      const w = new Readable();
      const writeCalled = deferredPromise();
      w.write = function (buffer) {
        written = true;
        strictEqual(ended, false);
        strictEqual(buffer.toString(), 'foo');
        writeCalled.resolve();
      }
      const endCalled = deferredPromise();
      w.end = function () {
        ended = true;
        strictEqual(written, true);
        endCalled.resolve();
      };
      r.pipe(w);
      await Promise.all([
        endCalled.promise,
        writeCalled.promise,
      ]);
    }
    {
      // Verify synchronous _read ending
      const r = new Readable();
      let called = false;
      r._read = function (n) {
        r.push(null);
      }
      r.once('end', function () {
        // Verify that this is called before the next tick
        called = true;
      })
      r.read();
      queueMicrotask(function () {
        strictEqual(called, true)
      })
    }
    {
      // Verify that adding readable listeners trigger data flow
      const r = new Readable({
        highWaterMark: 5
      });
      let onReadable = false;
      let readCalled = 0;
      r._read = function (n) {
        if (readCalled++ === 2) r.push(null);
        else r.push(Buffer.from('asdf'));
      }
      r.on('readable', function () {
        onReadable = true;
        r.read();
      });
      const endCalled = deferredPromise();
      r.on(
        'end',
        function () {
          strictEqual(readCalled, 3);
          ok(onReadable);
          endCalled.resolve();
        }
      );
      await endCalled.promise;
    }
    {
      // Verify that streams are chainable
      const r = new Readable();
      const readCalled = deferredPromise();
      r._read = readCalled.resolve;
      const r2 = r.setEncoding('utf8').pause().resume().pause();
      strictEqual(r, r2);
      await readCalled.promise;
    }
    {
      // Verify readableEncoding property
      ok(Reflect.has(Readable.prototype, 'readableEncoding'));
      const r = new Readable({
        encoding: 'utf8'
      });
      strictEqual(r.readableEncoding, 'utf8');
    }
    {
      // Verify readableObjectMode property
      ok(Reflect.has(Readable.prototype, 'readableObjectMode'));
      const r = new Readable({
        objectMode: true
      });
      strictEqual(r.readableObjectMode, true);
    }
    {
      // Verify writableObjectMode property
      ok(Reflect.has(Writable.prototype, 'writableObjectMode'));
      const w = new Writable({
        objectMode: true
      });
      strictEqual(w.writableObjectMode, true);
    }
  }
};

export const stream2_base64_single_char_read_end = {
  async test(ctrl, env, ctx) {
    const src = new Readable({
      encoding: 'base64'
    });
    const dst = new Writable();
    let hasRead = false;
    const accum = [];
    src._read = function (n) {
      if (!hasRead) {
        hasRead = true;
        queueMicrotask(function () {
          src.push(Buffer.from('1'));
          src.push(null);
        });
      }
    };
    dst._write = function (chunk, enc, cb) {
      accum.push(chunk);
      cb();
    };
    src.on('end', function () {
      strictEqual(String(Buffer.concat(accum)), 'MQ==');
      clearTimeout(timeout);
    })
    src.pipe(dst);
    const timeout = setTimeout(function () {
      fail('timed out waiting for _write');
    }, 100);
  }
};

export const writev = {
  async test(ctrl, env, ctx) {
    const queue = [];
    for (let decode = 0; decode < 2; decode++) {
      for (let uncork = 0; uncork < 2; uncork++) {
        for (let multi = 0; multi < 2; multi++) {
          queue.push([!!decode, !!uncork, !!multi]);
        }
      }
    }
    run();
    function run() {
      const t = queue.pop();
      if (t) test(t[0], t[1], t[2], run);
    }
    function test(decode, uncork, multi, next) {
      let counter = 0;
      let expectCount = 0;
      function cnt(msg) {
        expectCount++;
        const expect = expectCount;
        return function (er) {
          ifError(er);
          counter++;
          strictEqual(counter, expect);
        }
      }
      const w = new Writable({
        decodeStrings: decode
      });
      w._write = () => { throw new Error('Should not call _write'); };
      const expectChunks = decode
        ? [
            {
              encoding: 'buffer',
              chunk: [104, 101, 108, 108, 111, 44, 32]
            },
            {
              encoding: 'buffer',
              chunk: [119, 111, 114, 108, 100]
            },
            {
              encoding: 'buffer',
              chunk: [33]
            },
            {
              encoding: 'buffer',
              chunk: [10, 97, 110, 100, 32, 116, 104, 101, 110, 46, 46, 46]
            },
            {
              encoding: 'buffer',
              chunk: [250, 206, 190, 167, 222, 173, 190, 239, 222, 202, 251, 173]
            }
          ]
        : [
            {
              encoding: 'ascii',
              chunk: 'hello, '
            },
            {
              encoding: 'utf8',
              chunk: 'world'
            },
            {
              encoding: 'buffer',
              chunk: [33]
            },
            {
              encoding: 'latin1',
              chunk: '\nand then...'
            },
            {
              encoding: 'hex',
              chunk: 'facebea7deadbeefdecafbad'
            }
          ];
      let actualChunks;
      w._writev = function (chunks, cb) {
        actualChunks = chunks.map(function (chunk) {
          return {
            encoding: chunk.encoding,
            chunk: Buffer.isBuffer(chunk.chunk) ? Array.prototype.slice.call(chunk.chunk) : chunk.chunk
          };
        });
        cb();
      }
      w.cork();
      w.write('hello, ', 'ascii', cnt('hello'));
      w.write('world', 'utf8', cnt('world'));
      if (multi) w.cork();
      w.write(Buffer.from('!'), 'buffer', cnt('!'));
      w.write('\nand then...', 'latin1', cnt('and then'));
      if (multi) w.uncork();
      w.write('facebea7deadbeefdecafbad', 'hex', cnt('hex'));
      if (uncork) w.uncork();
      w.end(cnt('end'));
      w.on('finish', function () {
        // Make sure finish comes after all the write cb
        cnt('finish')();
        deepStrictEqual(actualChunks, expectChunks);
        next();
      });
    }
    {
      const writeCalled = deferredPromise();
      const writeFinished = deferredPromise();
      const w = new Writable({
        writev: function (chunks, cb) {
          cb();
          writeCalled.resolve();
        }
      })
      w.write('asd', writeFinished.resolve);
      await Promise.all([
        writeCalled.promise,
        writeFinished.promise,
      ]);
    }
  }
};

export const writeFinal = {
  async test(ctrl, env, ctx) {
    const finalCalled = deferredPromise();
    const finishCalled = deferredPromise();
    let shutdown = false;
    const w = new Writable({
      final: function (cb) {
        strictEqual(this, w);
        setTimeout(function () {
          shutdown = true;
          cb();
          finalCalled.resolve();
        }, 100);
      },
      write: function (chunk, e, cb) {
        queueMicrotask(cb);
      }
    })
    w.on(
      'finish',
      function () {
        ok(shutdown);
        finishCalled.resolve();
      }
    )
    w.write(Buffer.allocUnsafe(1));
    w.end(Buffer.allocUnsafe(0));
    await Promise.all([
      finalCalled.promise,
      finishCalled.promise,
    ]);
  }
};

export const writeDrain = {
  async test(ctrl, env, ctx) {
    const w = new Writable({
      write(data, enc, cb) {
        queueMicrotask(cb)
      },
      highWaterMark: 1
    });
    w.on('drain', () => { throw new Error('should not be called'); });
    w.write('asd');
    w.end();
  }
};

export const writeDestroy = {
  async test(ctrl, env, ctx) {
    for (const withPendingData of [false, true]) {
      for (const useEnd of [false, true]) {
        const callbacks = [];
        const w = new Writable({
          write(data, enc, cb) {
            callbacks.push(cb);
          },
          // Effectively disable the HWM to observe 'drain' events more easily.
          highWaterMark: 1
        });
        let chunksWritten = 0;
        let drains = 0;
        w.on('drain', () => drains++);
        function onWrite(err) {
          if (err) {
            strictEqual(w.destroyed, true);
            strictEqual(err.code, 'ERR_STREAM_DESTROYED');
          } else {
            chunksWritten++;
          }
        };
        w.write('abc', onWrite);
        strictEqual(chunksWritten, 0);
        strictEqual(drains, 0);
        callbacks.shift()();
        strictEqual(chunksWritten, 1);
        strictEqual(drains, 1);
        if (withPendingData) {
          // Test 2 cases: There either is or is not data still in the write queue.
          // (The second write will never actually get executed either way.)
          w.write('def', onWrite);
        }
        if (useEnd) {
          // Again, test 2 cases: Either we indicate that we want to end the
          // writable or not.
          w.end('ghi', onWrite);
        } else {
          w.write('ghi', onWrite);
        }
        strictEqual(chunksWritten, 1);
        w.destroy();
        strictEqual(chunksWritten, 1);
        callbacks.shift()()
        strictEqual(chunksWritten, useEnd && !withPendingData ? 1 : 2);
        strictEqual(callbacks.length, 0);
        strictEqual(drains, 1);
      }
    }
  }
};

export const writableState_uncorked_bufferedRequestCount = {
  async test(ctrl, env, ctx) {
    const writable = new Writable();
    const writevCalled = deferredPromise();
    const writeCalled = deferredPromise();
    writable._writev = (chunks, cb) => {
      strictEqual(chunks.length, 2);
      cb();
      writevCalled.resolve();
    };
    writable._write = (chunk, encoding, cb) => {
      cb();
      writeCalled.resolve();
    };

    // first cork
    writable.cork();
    strictEqual(writable._writableState.corked, 1);
    strictEqual(writable._writableState.bufferedRequestCount, 0);

    // cork again
    writable.cork();
    strictEqual(writable._writableState.corked, 2);

    // The first chunk is buffered
    writable.write('first chunk');
    strictEqual(writable._writableState.bufferedRequestCount, 1);

    // First uncork does nothing
    writable.uncork();
    strictEqual(writable._writableState.corked, 1);
    strictEqual(writable._writableState.bufferedRequestCount, 1);
    queueMicrotask(uncork);

    // The second chunk is buffered, because we uncork at the end of tick
    writable.write('second chunk');
    strictEqual(writable._writableState.corked, 1);
    strictEqual(writable._writableState.bufferedRequestCount, 2);
    const uncorkCalled = deferredPromise();
    function uncork() {
      // Second uncork flushes the buffer
      writable.uncork()
      strictEqual(writable._writableState.corked, 0);
      strictEqual(writable._writableState.bufferedRequestCount, 0);

      // Verify that end() uncorks correctly
      writable.cork();
      writable.write('third chunk');
      writable.end();

      // End causes an uncork() as well
      strictEqual(writable._writableState.corked, 0);
      strictEqual(writable._writableState.bufferedRequestCount, 0);
      uncorkCalled.resolve();
    }

    await Promise.all([
      writevCalled.promise,
      writeCalled.promise,
      uncorkCalled.promise,
    ]);
  }
};

export const writeableState_ending = {
  async test(ctrl, env, ctx) {
    const writable = new Writable();
    function testStates(ending, finished, ended) {
      strictEqual(writable._writableState.ending, ending);
      strictEqual(writable._writableState.finished, finished);
      strictEqual(writable._writableState.ended, ended);
    }
    writable._write = (chunk, encoding, cb) => {
      // Ending, finished, ended start in false.
      testStates(false, false, false);
      cb();
    };
    writable.on('finish', () => {
      // Ending, finished, ended = true.
      testStates(true, true, true);
    });
    const result = writable.end('testing function end()', () => {
      // Ending, finished, ended = true.
      testStates(true, true, true);
    });

    // End returns the writable instance
    strictEqual(result, writable);

    // Ending, ended = true.
    // finished = false.
    testStates(true, false, true);
  }
};

export const writable_write_writev_finish = {
  async test(ctrl, env, ctx) {
    {
      const writable = new Writable();
      const errored = deferredPromise();
      writable._write = (chunks, encoding, cb) => {
        cb(new Error('write test error'));
      }
      writable.on('finish', errored.reject);
      writable.on('prefinish', errored.reject);
      writable.on(
        'error',
        (er) => {
          strictEqual(er.message, 'write test error');
          errored.resolve();
        }
      );
      writable.end('test');
      await errored.promise;
    }
    {
      const writable = new Writable();
      const errored = deferredPromise();
      writable._write = (chunks, encoding, cb) => {
        queueMicrotask(() => cb(new Error('write test error')));
      }
      writable.on('finish', errored.reject);
      writable.on('prefinish', errored.reject)
      writable.on(
        'error',
        (er) => {
          strictEqual(er.message, 'write test error');
          errored.resolve();
        }
      )
      writable.end('test');
      await errored.promise;
    }
    {
      const writable = new Writable();
      const errored = deferredPromise();
      writable._write = (chunks, encoding, cb) => {
        cb(new Error('write test error'));
      };
      writable._writev = (chunks, cb) => {
        cb(new Error('writev test error'));
      };
      writable.on('finish', errored.reject);
      writable.on('prefinish', errored.reject);
      writable.on(
        'error',
        (er) => {
          strictEqual(er.message, 'writev test error');
          errored.resolve();
        }
      );
      writable.cork();
      writable.write('test');
      queueMicrotask(function () {
        writable.end('test');
      });
      await errored.promise;
    }
    {
      const writable = new Writable();
      const errored = deferredPromise();
      writable._write = (chunks, encoding, cb) => {
        queueMicrotask(() => cb(new Error('write test error')));
      }
      writable._writev = (chunks, cb) => {
        queueMicrotask(() => cb(new Error('writev test error')));
      }
      writable.on('finish', errored.reject);
      writable.on('prefinish', errored.reject);
      writable.on(
        'error',
        (er) => {
          strictEqual(er.message, 'writev test error');
          errored.resolve();
        }
      )
      writable.cork();
      writable.write('test');
      queueMicrotask(function () {
        writable.end('test');
      });
      await errored.promise;
    }

    // Regression test for
    // https://github.com/nodejs/node/issues/13812

    {
      const rs = new Readable();
      rs.push('ok');
      rs.push(null);
      rs._read = () => {};
      const ws = new Writable();
      const errored = deferredPromise();
      ws.on('finish', errored.reject);
      ws.on('error', errored.resolve);
      ws._write = (chunk, encoding, done) => {
        queueMicrotask(() => done(new Error()));
      }
      rs.pipe(ws);
      await errored.promise;
    }
    {
      const rs = new Readable();
      rs.push('ok');
      rs.push(null);
      rs._read = () => {};
      const ws = new Writable();
      const errored = deferredPromise();
      ws.on('finish', errored.reject);
      ws.on('error', errored.resolve);
      ws._write = (chunk, encoding, done) => {
        done(new Error());
      }
      rs.pipe(ws);
      await errored.promise;
    }
    {
      const w = new Writable();
      w._write = (chunk, encoding, cb) => {
        queueMicrotask(cb);
      }
      const errored = deferredPromise();
      w.on('error', errored.resolve);
      w.on('finish', errored.reject)
      w.on('prefinish', () => {
        w.write("shouldn't write in prefinish listener");
      })
      w.end();
      await errored.promise;
    }
    {
      const w = new Writable();
      w._write = (chunk, encoding, cb) => {
        queueMicrotask(cb);
      };
      const errored = deferredPromise();
      w.on('error', errored.resolve);
      w.on('finish', () => {
        w.write("shouldn't write in finish listener");
      })
      w.end();
      await errored.promise;
    }
  }
};

export const writable_write_error = {
  async test(ctrl, env, ctx) {
    async function expectError(w, args, code, sync) {
      if (sync) {
        if (code) {
          throws(() => w.write(...args), {
            code
          });
        } else {
          w.write(...args);
        }
      } else {
        let ticked = false;
        const writeCalled = deferredPromise();
        const errorCalled = deferredPromise();
        w.write(
          ...args,
          (err) => {
            strictEqual(ticked, true);
            strictEqual(err.code, code);
            writeCalled.resolve();
          }
        );
        ticked = true;
        w.on(
          'error',
          (err) => {
            strictEqual(err.code, code);
            errorCalled.resolve();
          }
        );
        await Promise.all([
          writeCalled.promise,
          errorCalled.promise,
        ]);
      }
    }
    async function test(autoDestroy) {
      {
        const w = new Writable({
          autoDestroy,
          _write() {}
        });
        w.end();
        await expectError(w, ['asd'], 'ERR_STREAM_WRITE_AFTER_END');
      }
      {
        const w = new Writable({
          autoDestroy,
          _write() {}
        });
        w.destroy();
      }
      {
        const w = new Writable({
          autoDestroy,
          _write() {}
        });
        await expectError(w, [null], 'ERR_STREAM_NULL_VALUES', true);
      }
      {
        const w = new Writable({
          autoDestroy,
          _write() {}
        });
        await expectError(w, [{}], 'ERR_INVALID_ARG_TYPE', true);
      }
      {
        const w = new Writable({
          decodeStrings: false,
          autoDestroy,
          _write() {}
        });
        await expectError(w, ['asd', 'noencoding'], 'ERR_UNKNOWN_ENCODING', true);
      }
    }
    await test(false)
    await test(true)
  }
};

export const writable_write_cb_twice = {
  async test(ctrl, env, ctx) {
    {
      // Sync + Sync
      const writeCalled = deferredPromise();
      const errored = deferredPromise();
      const writable = new Writable({
        write: (buf, enc, cb) => {
          cb();
          cb();
          writeCalled.resolve();
        }
      });
      writable.write('hi');
      writable.on(
        'error',
        function(err) {
          strictEqual(err.code, 'ERR_MULTIPLE_CALLBACK');
          errored.resolve();
        }
      );
      await Promise.all([
        writeCalled.promise,
        errored.promise,
      ]);
    }
    {
      // Sync + Async
      const writeCalled = deferredPromise();
      const errored = deferredPromise();
      const writable = new Writable({
        write: (buf, enc, cb) => {
          cb();
          queueMicrotask(() => {
            cb();
            writeCalled.resolve();
          });
        }
      })
      writable.write('hi');
      writable.on(
        'error',
        function (err) {
          strictEqual(err.code, 'ERR_MULTIPLE_CALLBACK');
          errored.resolve();
        }
      );
      await Promise.all([
        writeCalled.promise,
        errored.promise,
      ]);
    }
    {
      // Async + Async
      const writeCalled = deferredPromise();
      const errored = deferredPromise();
      const writable = new Writable({
        write: (buf, enc, cb) => {
          queueMicrotask(cb);
          queueMicrotask(() => {
            cb();
            writeCalled.resolve();
          });
        }
      });
      writable.write('hi');
      writable.on(
        'error',
        function (err) {
          strictEqual(err.code, 'ERR_MULTIPLE_CALLBACK');
          errored.resolve();
        }
      );
      await Promise.all([
        writeCalled.promise,
        errored.promise,
      ]);
    }
  }
};

export const writable_write_cb_error = {
  async test(ctrl, env, ctx) {
    {
      let callbackCalled = false;
      // Sync Error
      const writeCalled = deferredPromise();
      const errored = deferredPromise();
      const writeFinished = deferredPromise();
      const writable = new Writable({
        write: (buf, enc, cb) => {
          cb(new Error());
          writeCalled.resolve();
        }
      });
      writable.on(
        'error',
        () => {
          strictEqual(callbackCalled, true);
          errored.resolve();
        }
      );
      writable.write(
        'hi',
        () => {
          callbackCalled = true;
          writeFinished.resolve();
        }
      );
      await Promise.all([
        writeCalled.promise,
        errored.promise,
        writeFinished.promise,
      ]);
    }
    {
      let callbackCalled = false;
      // Async Error
      const writeCalled = deferredPromise();
      const errored = deferredPromise();
      const writeFinished = deferredPromise();
      const writable = new Writable({
        write: (buf, enc, cb) => {
          queueMicrotask(() => cb(new Error()));
          writeCalled.resolve();
        }
      });
      writable.on(
        'error',
        () => {
          strictEqual(callbackCalled, true);
          errored.resolve();
        }
      );
      writable.write(
        'hi',
        () => {
          callbackCalled = true;
          writeFinished.resolve();
        }
      );
      await Promise.all([
        writeCalled.promise,
        errored.promise,
        writeFinished.promise,
      ]);
    }
    {
      // Sync Error
      const errored = deferredPromise();
      const writeCalled = deferredPromise();
      const writable = new Writable({
        write: (buf, enc, cb) => {
          cb(new Error());
          writeCalled.resolve();
        }
      });
      writable.on('error', errored.resolve);
      let cnt = 0;
      // Ensure we don't live lock on sync error
      while (writable.write('a')) cnt++;
      strictEqual(cnt, 0);
      await Promise.all([
        writeCalled.promise,
        errored.promise,
      ]);
    }
  }
};

export const writable_writable = {
  async test(ctrl, env, ctx) {
    {
      const w = new Writable({
        write() {}
      });
      strictEqual(w.writable, true);
      w.destroy();
      strictEqual(w.writable, false);
    }
    {
      const writeCalled = deferredPromise();
      const errored = deferredPromise();
      const w = new Writable({
        write: (chunk, encoding, callback) => {
          callback(new Error());
          writeCalled.resolve();
        }
      })
      strictEqual(w.writable, true);
      w.write('asd');
      strictEqual(w.writable, false);
      w.on('error', errored.resolve);
      await Promise.all([
        writeCalled.promise,
        errored.promise,
      ]);
    }
    {
      const writeCalled = deferredPromise();
      const errored = deferredPromise();
      const w = new Writable({
        write: (chunk, encoding, callback) => {
          queueMicrotask(() => {
            callback(new Error())
            strictEqual(w.writable, false);
            writeCalled.resolve();
          })
        }
      });
      w.write('asd');
      w.on('error', errored.resolve);
      await Promise.all([
        writeCalled.promise,
        errored.promise,
      ]);
    }
    {
      const closed = deferredPromise();
      const w = new Writable({
        write: closed.reject
      })
      w.on('close', closed.resolve);
      strictEqual(w.writable, true);
      w.end()
      strictEqual(w.writable, false);
      await closed.promise;
    }
  }
};

export const writable_properties = {
  test(ctrl, env, ctx) {
    {
      const w = new Writable();
      strictEqual(w.writableCorked, 0);
      w.uncork();
      strictEqual(w.writableCorked, 0);
      w.cork();
      strictEqual(w.writableCorked, 1);
      w.cork();
      strictEqual(w.writableCorked, 2);
      w.uncork();
      strictEqual(w.writableCorked, 1);
      w.uncork();
      strictEqual(w.writableCorked, 0);
      w.uncork();
      strictEqual(w.writableCorked, 0);
    }

  }
};

export const writable_null = {
  async test(ctrl, env, ctx) {
    class MyWritable extends Writable {
      constructor(options) {
        super({
          autoDestroy: false,
          ...options
        });
      }
      _write(chunk, encoding, callback) {
        notStrictEqual(chunk, null);
        callback();
      }
    }
    {
      const m = new MyWritable({
        objectMode: true
      });
      m.on('error', () => { throw new Error('should not be called') });
      throws(
        () => {
          m.write(null);
        },
        {
          code: 'ERR_STREAM_NULL_VALUES'
        }
      );
    }
    {
      const m = new MyWritable();
      m.on('error', () => { throw new Error('should not be called') });
      throws(
        () => {
          m.write(false);
        },
        {
          code: 'ERR_INVALID_ARG_TYPE'
        }
      );
    }
    {
      // Should not throw.
      const m = new MyWritable({
        objectMode: true
      });
      m.write(false, ifError);
    }
    {
      // Should not throw.
      const m = new MyWritable({
        objectMode: true
      }).on('error', (e) => {
        ifError(e || new Error('should not get here'));
      })
      m.write(false, ifError);
    }
  }
};

export const writable_needdrain_state = {
  async test(ctrl, env, ctx) {
    const transform = new Transform({
      transform: _transform,
      highWaterMark: 1
    })
    const transformCalled = deferredPromise();
    const writeFinished = deferredPromise();
    function _transform(chunk, encoding, cb) {
      queueMicrotask(() => {
        strictEqual(transform._writableState.needDrain, true);
        cb();
        transformCalled.resolve();
      })
    }
    strictEqual(transform._writableState.needDrain, false);
    transform.write(
      'asdasd',
      () => {
        strictEqual(transform._writableState.needDrain, false);
        writeFinished.resolve();
      }
    )
    strictEqual(transform._writableState.needDrain, true);
    await Promise.all([
      transformCalled.promise,
      writeFinished.promise,
    ]);
  }
};

export const writable_invalid_chunk = {
  test(ctrl, env, ctx) {
    function testWriteType(val, objectMode, code) {
      const writable = new Writable({
        objectMode,
        write: () => {}
      });
      writable.on('error', () => { throw new Error('should not have been called'); });
      if (code) {
        throws(
          () => {
            writable.write(val);
          },
          {
            code
          }
        );
      } else {
        writable.write(val);
      }
    }
    testWriteType([], false, 'ERR_INVALID_ARG_TYPE');
    testWriteType({}, false, 'ERR_INVALID_ARG_TYPE');
    testWriteType(0, false, 'ERR_INVALID_ARG_TYPE');
    testWriteType(true, false, 'ERR_INVALID_ARG_TYPE');
    testWriteType(0.0, false, 'ERR_INVALID_ARG_TYPE');
    testWriteType(undefined, false, 'ERR_INVALID_ARG_TYPE');
    testWriteType(null, false, 'ERR_STREAM_NULL_VALUES');
    testWriteType([], true);
    testWriteType({}, true);
    testWriteType(0, true);
    testWriteType(true, true);
    testWriteType(0.0, true);
    testWriteType(undefined, true);
    testWriteType(null, true, 'ERR_STREAM_NULL_VALUES');
  }
};

export const writable_finished = {
  async test(ctrl, env, ctx) {
    // basic
    {
      // Find it on Writable.prototype
      ok(Reflect.has(Writable.prototype, 'writableFinished'));
    }

    // event
    {
      const writable = new Writable();
      writable._write = (chunk, encoding, cb) => {
        // The state finished should start in false.
        strictEqual(writable.writableFinished, false);
        cb();
      };
      const finishCalled = deferredPromise();
      const endCalled = deferredPromise();
      writable.on(
        'finish',
        () => {
          strictEqual(writable.writableFinished, true);
          finishCalled.resolve();
        }
      );
      writable.end(
        'testing finished state',
        () => {
          strictEqual(writable.writableFinished, true);
          endCalled.resolve();
        }
      );
      await Promise.all([
        finishCalled.promise,
        endCalled.promise,
      ]);
    }
    {
      // Emit finish asynchronously.

      const w = new Writable({
        write(chunk, encoding, cb) {
          cb();
        }
      });
      w.end();
      const finishCalled = deferredPromise();
      w.on('finish', finishCalled.resolve);
      await finishCalled.promise;
    }
    {
      // Emit prefinish synchronously.

      const w = new Writable({
        write(chunk, encoding, cb) {
          cb();
        }
      })
      let sync = true;
      w.on(
        'prefinish',
        () => {
          strictEqual(sync, true);
        }
      )
      w.end();
      sync = false;
    }
    {
      // Emit prefinish synchronously w/ final.

      const w = new Writable({
        write(chunk, encoding, cb) {
          cb();
        },
        final(cb) {
          cb();
        }
      })
      let sync = true;
      w.on(
        'prefinish',
        () => {
          strictEqual(sync, true);
        }
      )
      w.end();
      sync = false;
    }
    {
      // Call _final synchronously.

      let sync = true
      const w = new Writable({
        write(chunk, encoding, cb) {
          cb();
        },
        final: (cb) => {
          strictEqual(sync, true);
          cb();
        }
      });
      w.end();
      sync = false;
    }
  }
};

export const writable_finished_state = {
  async test(ctrl, env, ctx) {
    const writable = new Writable()
    writable._write = (chunk, encoding, cb) => {
      // The state finished should start in false.
      strictEqual(writable._writableState.finished, false);
      cb();
    };
    const finishCalled = deferredPromise();
    const endCalled = deferredPromise();
    writable.on(
      'finish',
      () => {
        strictEqual(writable._writableState.finished, true);
        finishCalled.resolve();
      }
    );
    writable.end(
      'testing finished state',
      () => {
        strictEqual(writable._writableState.finished, true);
        endCalled.resolve();
      }
    );
    await Promise.all([
      finishCalled.promise,
      endCalled.promise,
    ]);
  }
};

export const writable_finish_destroyed = {
  async test(ctrl, env, ctx) {
    {
      const writeCalled = deferredPromise();
      const closed = deferredPromise();
      const w = new Writable({
        write: (chunk, encoding, cb) => {
          w.on(
            'close',
            () => {
              cb();
              writeCalled.resolve();
            }
          );
        }
      });
      w.on('close', closed.resolve);
      w.on('finish', closed.reject);
      w.end('asd');
      w.destroy();
      await Promise.all([
        writeCalled.promise,
        closed.promise,
      ]);
    }
    {
      const writeCalled = deferredPromise();
      const closed = deferredPromise();
      const w = new Writable({
        write: (chunk, encoding, cb) => {
          w.on(
            'close',
            () => {
              cb();
              w.end();
              writeCalled.resolve();
            }
          );
        }
      });
      w.on('finish', closed.reject);
      w.on('close', closed.resolve);
      w.write('asd');
      w.destroy();
      await Promise.all([
        writeCalled.promise,
        closed.promise,
      ]);
    }
    {
      const w = new Writable({
        write() {}
      })
      const closed = deferredPromise();
      w.on('finish', closed.reject);
      w.on('close', closed.resolve);
      w.end();
      w.destroy();
      await closed.promise;
    }
  }
};

export const writable_final_throw = {
  async test(ctrl, env, ctx) {
    class Foo extends Duplex {
      _final(callback) {
        throw new Error('fhqwhgads');
      }
      _read() {}
    }
    const writeCalled = deferredPromise();
    const endFinished = deferredPromise();
    const errored = deferredPromise();
    const foo = new Foo();
    foo._write = (chunk, encoding, cb) => {
      cb()
      writeCalled.resolve();
    };
    foo.end(
      'test',
      function(err) {
        strictEqual(err.message, 'fhqwhgads');
        endFinished.resolve();
      }
    );
    foo.on('error', errored.resolve);
    await Promise.all([
      writeCalled.promise,
      endFinished.promise,
      errored.promise,
    ]);
  }
};

export const writable_final_destroy = {
  async test(ctrl, env, ctx) {
    const w = new Writable({
      write(chunk, encoding, callback) {
        callback(null)
      },
      final(callback) {
        queueMicrotask(callback)
      }
    })
    const closed = deferredPromise();
    w.end()
    w.destroy()
    w.on('prefinish', closed.reject);
    w.on('finish', closed.reject);
    w.on('close', closed.resolve);
    await closed.promise;
  }
};

export const writable_final_async = {
  async test(ctrl, env, ctx) {
    {
      class Foo extends Duplex {
        async _final(callback) {
          await scheduler.wait(10);
          callback();
        }
        _read() {}
      }
      const foo = new Foo();
      const writeCalled = deferredPromise();
      const endCalled = deferredPromise();
      foo._write = (chunk, encoding, cb) => {
        cb();
        writeCalled.resolve();
      };
      foo.end('test', endCalled.resolve);
      foo.on('error', endCalled.reject);
      await Promise.all([
        endCalled.promise,
        writeCalled.promise,
      ]);
    }
  }
};

export const writable_ended_state = {
  async test(ctrl, env, ctx) {
    const writable = new Writable();
    const writeCalled = deferredPromise();
    const endCalled = deferredPromise();
    writable._write = (chunk, encoding, cb) => {
      strictEqual(writable._writableState.ended, false);
      strictEqual(writable._writableState.writable, undefined);
      strictEqual(writable.writableEnded, false);
      cb();
      writeCalled.resolve();
    }
    strictEqual(writable._writableState.ended, false);
    strictEqual(writable._writableState.writable, undefined);
    strictEqual(writable.writable, true);
    strictEqual(writable.writableEnded, false);
    writable.end(
      'testing ended state',
      () => {
        strictEqual(writable._writableState.ended, true);
        strictEqual(writable._writableState.writable, undefined);
        strictEqual(writable.writable, false);
        strictEqual(writable.writableEnded, true);
        endCalled.resolve();
      }
    )
    strictEqual(writable._writableState.ended, true);
    strictEqual(writable._writableState.writable, undefined);
    strictEqual(writable.writable, false);
    strictEqual(writable.writableEnded, true);
    await Promise.all([
      writeCalled.promise,
      endCalled.promise,
    ]);
  }
};

export const writable_end_multiple = {
  async test(ctrl, env, ctx) {
    const writable = new Writable();
    writable._write = (chunk, encoding, cb) => {
      setTimeout(() => cb(), 10);
    }
    const endCalled1 = deferredPromise();
    const endCalled2 = deferredPromise();
    const finishCalled = deferredPromise();
    writable.end('testing ended state', endCalled1.resolve);
    writable.end(endCalled2.resolve);
    writable.on(
      'finish',
      () => {
        let ticked = false;
        writable.end(
          (err) => {
            strictEqual(ticked, true);
            strictEqual(err.code, 'ERR_STREAM_ALREADY_FINISHED');
            finishCalled.resolve();
          }
        );
        ticked = true;
      }
    );
    await Promise.all([
      endCalled1.promise,
      endCalled2.promise,
      finishCalled.promise,
    ]);
  }
};

export const writable_end_cb_error = {
  async test(ctrl, env, ctx) {
    {
      // Invoke end callback on failure.
      const writable = new Writable();
      const _err = new Error('kaboom');
      writable._write = (chunk, encoding, cb) => {
        queueMicrotask(() => cb(_err));
      }
      const errored = deferredPromise();
      const endCalled1 = deferredPromise();
      const endCalled2 = deferredPromise();
      writable.on(
        'error',
        (err) => {
          strictEqual(err, _err);
          errored.resolve();
        }
      )
      writable.write('asd');
      writable.end(
        (err) => {
          strictEqual(err, _err);
          endCalled1.resolve();
        }
      );
      writable.end(
        (err) => {
          strictEqual(err, _err);
          endCalled2.resolve();
        }
      );
      await Promise.all([
        errored.promise,
        endCalled1.promise,
        endCalled2.promise,
      ]);
    }
    {
      // Don't invoke end callback twice
      const writable = new Writable();
      writable._write = (chunk, encoding, cb) => {
        queueMicrotask(cb);
      };
      let called = false;
      const endCalled = deferredPromise();
      const errored = deferredPromise();
      const finishCalled = deferredPromise();
      writable.end(
        'asd',
        (err) => {
          called = true;
          strictEqual(err, undefined);
          endCalled.resolve();
        }
      );
      writable.on(
        'error',
        (err) => {
          strictEqual(err.message, 'kaboom');
          errored.resolve();
        }
      );
      writable.on(
        'finish',
        () => {
          strictEqual(called, true);
          writable.emit('error', new Error('kaboom'));
          finishCalled.resolve();
        }
      );
      await Promise.all([
        endCalled.promise,
        errored.promise,
        finishCalled.promise,
      ]);
    }
    {
      const w = new Writable({
        write(chunk, encoding, callback) {
          queueMicrotask(callback);
        },
        finish(callback) {
          queueMicrotask(callback);
        }
      });
      const endCalled1 = deferredPromise();
      const endCalled2 = deferredPromise();
      const endCalled3 = deferredPromise();
      const errored = deferredPromise();
      w.end(
        'testing ended state',
        (err) => {
          strictEqual(err.code, 'ERR_STREAM_WRITE_AFTER_END');
          endCalled1.resolve();
        }
      )
      strictEqual(w.destroyed, false);
      strictEqual(w.writableEnded, true);
      w.end(
        (err) => {
          strictEqual(err.code, 'ERR_STREAM_WRITE_AFTER_END');
          endCalled2.resolve();
        }
      )
      strictEqual(w.destroyed, false);
      strictEqual(w.writableEnded, true);
      w.end(
        'end',
        (err) => {
          strictEqual(err.code, 'ERR_STREAM_WRITE_AFTER_END');
          endCalled3.resolve();
        }
      )
      strictEqual(w.destroyed, true);
      w.on(
        'error',
        (err) => {
          strictEqual(err.code, 'ERR_STREAM_WRITE_AFTER_END');
          errored.resolve();
        }
      )
      w.on('finish', errored.reject);
      await Promise.all([
        endCalled1.promise,
        endCalled2.promise,
        endCalled3.promise,
        errored.promise,
      ]);
    }
  }
};

export const writable_destroy = {
  async test(ctrl, env, ctx) {
    {
      const write = new Writable({
        write(chunk, enc, cb) {
          cb();
        }
      });
      const closed = deferredPromise();
      write.on('finish', closed.reject);
      write.on('close', closed.resolve);
      write.destroy();
      strictEqual(write.destroyed, true);
      await closed.promise;
    }
    {
      const write = new Writable({
        write(chunk, enc, cb) {
          this.destroy(new Error('asd'));
          cb();
        }
      })
      const errored = deferredPromise();
      write.on('error', errored.resolve);
      write.on('finish', errored.reject);
      write.end('asd');
      strictEqual(write.destroyed, true);
      await errored.promise;
    }
    {
      const write = new Writable({
        write(chunk, enc, cb) {
          cb()
        }
      });
      const expected = new Error('kaboom');
      const errored = deferredPromise();
      const closed = deferredPromise();
      write.on('finish', closed.reject);
      write.on('close', closed.resolve);
      write.on(
        'error',
        (err) => {
          strictEqual(err, expected);
          errored.resolve();
        }
      )
      write.destroy(expected);
      strictEqual(write.destroyed, true);
      await Promise.all([
        errored.promise,
        closed.promise,
      ]);
    }
    {
      const write = new Writable({
        write(chunk, enc, cb) {
          cb();
        }
      });
      write._destroy = function (err, cb) {
        strictEqual(err, expected);
        cb(err);
      }
      const errored = deferredPromise();
      const closed = deferredPromise();
      const expected = new Error('kaboom');
      write.on('finish', closed.reject);
      write.on('close', closed.resolve);
      write.on(
        'error',
        (err) => {
          strictEqual(err, expected);
          errored.resolve();
        }
      )
      write.destroy(expected);
      strictEqual(write.destroyed, true);
      await Promise.all([
        closed.promise,
        errored.promise,
      ]);
    }
    {
      const closed = deferredPromise();
      const destroyCalled = deferredPromise();
      const write = new Writable({
        write(chunk, enc, cb) {
          cb();
        },
        destroy: function (err, cb) {
          strictEqual(err, expected);
          cb();
          destroyCalled.resolve();
        }
      });
      const expected = new Error('kaboom');
      write.on('finish', closed.reject);
      write.on('close', closed.resolve);

      // Error is swallowed by the custom _destroy
      write.on('error', closed.reject);
      write.destroy(expected);
      strictEqual(write.destroyed, true);
      await Promise.all([
        destroyCalled.promise,
        closed.promise,
      ]);
    }
    {
      const write = new Writable({
        write(chunk, enc, cb) {
          cb();
        }
      });
      const destroyCalled = deferredPromise();
      write._destroy = function (err, cb) {
        strictEqual(err, null);
        cb();
        destroyCalled.resolve();
      };
      write.destroy();
      strictEqual(write.destroyed, true);
      await destroyCalled.promise;
    }
    {
      const write = new Writable({
        write(chunk, enc, cb) {
          cb();
        }
      })
      const destroyCalled = deferredPromise();
      const closed = deferredPromise();
      write._destroy = function (err, cb) {
        strictEqual(err, null);
        queueMicrotask(() => {
          this.end();
          cb();
          destroyCalled.resolve();
        })
      };
      write.on('finish', closed.reject);
      write.on('close', closed.resolve);
      write.destroy();
      strictEqual(write.destroyed, true);
      await Promise.all([
        destroyCalled.promise,
        closed.promise,
      ]);
    }
    {
      const write = new Writable({
        write(chunk, enc, cb) {
          cb();
        }
      });
      const expected = new Error('kaboom');
      const destroyCalled = deferredPromise();
      const closed = deferredPromise();
      const errored = deferredPromise();
      write._destroy = function (err, cb) {
        strictEqual(err, null);
        cb(expected);
        destroyCalled.resolve();
      };
      write.on('close', closed.resolve);
      write.on('finish', closed.reject);
      write.on(
        'error',
        (err) => {
          strictEqual(err, expected);
          errored.resolve();
        }
      )
      write.destroy();
      strictEqual(write.destroyed, true);
      await Promise.all([
        destroyCalled.promise,
        closed.promise,
        errored.promise,
      ]);
    }
    {
      // double error case
      const write = new Writable({
        write(chunk, enc, cb) {
          cb();
        }
      });
      let ticked = false;
      const writeFinished = deferredPromise();
      const errored = deferredPromise();
      write.on(
        'close',
        () => {
          strictEqual(ticked, true);
          writeFinished.resolve();
        }
      );
      write.on(
        'error',
        (err) => {
          strictEqual(ticked, true);
          strictEqual(err.message, 'kaboom 1');
          strictEqual(write._writableState.errorEmitted, true);
          errored.resolve();
        }
      );
      const expected = new Error('kaboom 1');
      write.destroy(expected);
      write.destroy(new Error('kaboom 2'));
      strictEqual(write._writableState.errored, expected);
      strictEqual(write._writableState.errorEmitted, false);
      strictEqual(write.destroyed, true);
      ticked = true;
      await Promise.all([
        writeFinished.promise,
        errored.promise,
      ]);
    }
    {
      const writable = new Writable({
        destroy: function (err, cb) {
          queueMicrotask(() => cb(new Error('kaboom 1')));
        },
        write(chunk, enc, cb) {
          cb();
        }
      });
      let ticked = false;
      const closed = deferredPromise();
      const errored = deferredPromise();
      writable.on(
        'close',
        () => {
          writable.on('error', () => { throw new Error('should not have been called') });
          writable.destroy(new Error('hello'));
          strictEqual(ticked, true);
          strictEqual(writable._writableState.errorEmitted, true);
          closed.resolve();
        }
      )
      writable.on(
        'error',
        (err) => {
          strictEqual(ticked, true);
          strictEqual(err.message, 'kaboom 1');
          strictEqual(writable._writableState.errorEmitted, true);
          errored.resolve();
        }
      )
      writable.destroy();
      strictEqual(writable.destroyed, true);
      strictEqual(writable._writableState.errored, null);
      strictEqual(writable._writableState.errorEmitted, false);

      // Test case where `writable.destroy()` is called again with an error before
      // the `_destroy()` callback is called.
      writable.destroy(new Error('kaboom 2'));
      strictEqual(writable._writableState.errorEmitted, false);
      strictEqual(writable._writableState.errored, null);
      ticked = true;

      await Promise.all([
        closed.promise,
        errored.promise,
      ]);
    }
    {
      const write = new Writable({
        write(chunk, enc, cb) {
          cb();
        }
      });
      write.destroyed = true;
      strictEqual(write.destroyed, true);

      // The internal destroy() mechanism should not be triggered
      write.on('close', () => { throw new Error('should not have been called'); });
      write.destroy();
    }
    {
      function MyWritable() {
        strictEqual(this.destroyed, false);
        this.destroyed = false;
        Writable.call(this);
      }
      Object.setPrototypeOf(MyWritable.prototype, Writable.prototype);
      Object.setPrototypeOf(MyWritable, Writable);
      new MyWritable();
    }
    {
      // Destroy and destroy callback
      const write = new Writable({
        write(chunk, enc, cb) {
          cb();
        }
      });
      write.destroy();
      const expected = new Error('kaboom');
      const destroyed = deferredPromise();
      write.destroy(
        expected,
        (err) => {
          strictEqual(err, undefined);
          destroyed.resolve();
        }
      );
      await destroyed.promise;
    }
    {
      // Checks that `._undestroy()` restores the state so that `final` will be
      // called again.
      const writeCalled = deferredPromise();
      const finalCalled = deferredPromise();
      const closed = deferredPromise();
      let finalCounter = 0;
      const write = new Writable({
        write: writeCalled.resolve(),
        final: (cb) => {
          cb();
          if (++finalCounter === 2) finalCalled.resolve();
        },
        autoDestroy: true
      })
      write.end();
      write.once(
        'close',
        () => {
          write._undestroy();
          write.end();
          closed.resolve();
        }
      );
      await Promise.all([
        writeCalled.promise,
        finalCalled.promise,
        closed.promise,
      ]);
    }
    {
      const write = new Writable();
      write.destroy();
      const writeFinished = deferredPromise();
      write.on('error', writeFinished.reject);
      write.write(
        'asd',
        function(err) {
          strictEqual(err.code, 'ERR_STREAM_DESTROYED');
          writeFinished.resolve();
        }
      );
      await writeFinished.promise;
    }
    {
      const write = new Writable({
        write(chunk, enc, cb) {
          cb();
        }
      });
      const writeFinished1 = deferredPromise();
      const writeFinished2 = deferredPromise();
      const writeFinished3 = deferredPromise();
      write.on('error', writeFinished1.reject);
      write.cork();
      write.write('asd', writeFinished1.resolve);
      write.uncork();
      write.cork();
      write.write(
        'asd',
        function(err) {
          strictEqual(err.code, 'ERR_STREAM_DESTROYED');
          writeFinished2.resolve();
        }
      );
      write.destroy();
      write.write(
        'asd',
        function(err) {
          strictEqual(err.code, 'ERR_STREAM_DESTROYED');
          writeFinished3.resolve();
        }
      );
      write.uncork();
      await Promise.all([
        writeFinished1.promise,
        writeFinished2.promise,
        writeFinished3.promise,
      ]);
    }
    {
      // Call end(cb) after error & destroy

      const write = new Writable({
        write(chunk, enc, cb) {
          cb(new Error('asd'));
        }
      });
      const errored = deferredPromise();
      write.on(
        'error',
        () => {
          write.destroy();
          let ticked = false;
          write.end(
            (err) => {
              strictEqual(ticked, true);
              strictEqual(err.code, 'ERR_STREAM_DESTROYED');
              errored.resolve();
            }
          )
          ticked = true;
        }
      )
      write.write('asd');
      await errored.promise;
    }
    {
      // Call end(cb) after finish & destroy

      const write = new Writable({
        write(chunk, enc, cb) {
          cb();
        }
      });
      const finishCalled = deferredPromise();
      write.on(
        'finish',
        () => {
          write.destroy();
          let ticked = false;
          write.end(
            (err) => {
              strictEqual(ticked, true);
              strictEqual(err.code, 'ERR_STREAM_ALREADY_FINISHED');
              finishCalled.resolve();
            }
          )
          ticked = true;
        }
      )
      write.end();
      await finishCalled.promise;
    }
    {
      // Call end(cb) after error & destroy and don't trigger
      // unhandled exception.

      const write = new Writable({
        write(chunk, enc, cb) {
          queueMicrotask(cb);
        }
      })
      const _err = new Error('asd');
      const errored = deferredPromise();
      const ended = deferredPromise();
      write.once(
        'error',
        (err) => {
          strictEqual(err.message, 'asd');
          errored.resolve();
        }
      )
      write.end(
        'asd',
        (err) => {
          strictEqual(err, _err);
          ended.resolve();
        }
      )
      write.destroy(_err);
      await Promise.all([
        errored.promise,
        ended.promise,
      ]);
    }
    {
      // Call buffered write callback with error

      const _err = new Error('asd');
      const write = new Writable({
        write(chunk, enc, cb) {
          queueMicrotask(() => cb(_err));
        },
        autoDestroy: false
      });
      write.cork();
      const writeFinished1 = deferredPromise();
      const writeFinished2 = deferredPromise();
      const errored = deferredPromise();
      write.write(
        'asd',
        (err) => {
          strictEqual(err, _err);
          writeFinished1.resolve();
        }
      );
      write.write(
        'asd',
        (err) => {
          strictEqual(err, _err);
          writeFinished2.resolve();
        }
      );
      write.on(
        'error',
        (err) => {
          strictEqual(err, _err);
          errored.resolve();
        }
      );
      write.uncork();
      await Promise.all([
        writeFinished1.promise,
        writeFinished2.promise,
        errored.promise,
      ]);
    }
    {
      // Ensure callback order.

      let state = 0;
      const write = new Writable({
        write(chunk, enc, cb) {
          // `queueMicrotask()` is used on purpose to ensure the callback is called
          // after `queueMicrotask()` callbacks.
          queueMicrotask(cb);
        }
      })
      const writeFinished1 = deferredPromise();
      const writeFinished2 = deferredPromise();
      write.write(
        'asd',
        () => {
          strictEqual(state++, 0);
          writeFinished1.resolve();
        }
      );
      write.write(
        'asd',
        (err) => {
          strictEqual(err.code, 'ERR_STREAM_DESTROYED');
          strictEqual(state++, 1);
          writeFinished2.resolve();
        }
      );
      write.destroy();
      await Promise.all([
        writeFinished1.promise,
        writeFinished2.promise,
      ]);
    }
    {
      const write = new Writable({
        autoDestroy: false,
        write(chunk, enc, cb) {
          cb();
          cb();
        }
      })
      const errored = deferredPromise();
      write.on(
        'error',
        () => {
          ok(write._writableState.errored);
          errored.resolve();
        }
      );
      write.write('asd');
      await errored.promise;
    }
    {
      const ac = new AbortController();
      const write = addAbortSignal(
        ac.signal,
        new Writable({
          write(chunk, enc, cb) {
            cb();
          }
        })
      );
      const errored = deferredPromise();
      write.on(
        'error',
        (e) => {
          strictEqual(e.name, 'AbortError');
          strictEqual(write.destroyed, true);
          errored.resolve();
        }
      )
      write.write('asd');
      ac.abort();
      await errored.promise;
    }
    {
      const ac = new AbortController();
      const write = new Writable({
        signal: ac.signal,
        write(chunk, enc, cb) {
          cb();
        }
      });
      const errored = deferredPromise();
      write.on(
        'error',
        (e) => {
          strictEqual(e.name, 'AbortError');
          strictEqual(write.destroyed, true);
          errored.resolve();
        }
      )
      write.write('asd');
      ac.abort();
      await errored.promise;
    }
    {
      const signal = AbortSignal.abort();
      const write = new Writable({
        signal,
        write(chunk, enc, cb) {
          cb();
        }
      });
      const errored = deferredPromise();
      write.on(
        'error',
        (e) => {
          strictEqual(e.name, 'AbortError');
          strictEqual(write.destroyed, true);
          errored.resolve();
        }
      );
      await errored.promise;
    }
    {
      // Destroy twice
      const write = new Writable({
        write(chunk, enc, cb) {
          cb();
        }
      })
      const ended = deferredPromise()
      write.end(ended.resolve);
      write.destroy();
      write.destroy();
      await ended.promise;
    }
    {
      // https://github.com/nodejs/node/issues/39356
      const s = new Writable({
        final() {}
      })
      const _err = new Error('oh no');
      // Remove `callback` and it works
      const ended = deferredPromise();
      const errored = deferredPromise();
      s.end(
        (err) => {
          strictEqual(err, _err);
          ended.resolve();
        }
      )
      s.on(
        'error',
        (err) => {
          strictEqual(err, _err);
          errored.resolve();
        }
      )
      s.destroy(_err);
      await Promise.all([
        errored.promise,
        ended.promise,
      ]);
    }
  }
};

export const writable_decoded_encoding = {
  async test(ctrl, env, ctx) {
    class MyWritable extends Writable {
      constructor(fn, options) {
        super(options);
        this.fn = fn;
      }
      _write(chunk, encoding, callback) {
        this.fn(Buffer.isBuffer(chunk), typeof chunk, encoding);
        callback();
      }
    }
    {
      const m = new MyWritable(
        function (isBuffer, type, enc) {
          ok(isBuffer);
          strictEqual(type, 'object');
          strictEqual(enc, 'buffer');
        },
        {
          decodeStrings: true
        }
      )
      m.write('some-text', 'utf8');
      m.end();
      await promises.finished(m);
    }
    {
      const m = new MyWritable(
        function (isBuffer, type, enc) {
          ok(!isBuffer);
          strictEqual(type, 'string');
          strictEqual(enc, 'utf8');
        },
        {
          decodeStrings: false
        }
      )
      m.write('some-text', 'utf8');
      m.end();
      await promises.finished(m);
    }
  }
};

export const writable_constructor_set_methods = {
  async test(ctrl, env, ctx) {
    const bufferBlerg = Buffer.from('blerg');
    const w = new Writable();
    throws(
      () => {
        w.end(bufferBlerg);
      },
      {
        name: 'Error',
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
        message: 'The _write() method is not implemented'
      }
    );
    const writeCalled = deferredPromise();
    const writevCalled = deferredPromise();
    const _write = (chunk, _, next) => {
      next();
      writeCalled.resolve();
    };
    const _writev = (chunks, next) => {
      strictEqual(chunks.length, 2);
      next();
      writevCalled.resolve();
    }
    const w2 = new Writable({
      write: _write,
      writev: _writev
    });
    strictEqual(w2._write, _write);
    strictEqual(w2._writev, _writev);
    w2.write(bufferBlerg);
    w2.cork();
    w2.write(bufferBlerg);
    w2.write(bufferBlerg);
    w2.end();
    await Promise.all([
      writeCalled.promise,
      writevCalled.promise,
    ]);
  }
};

export const writable_clear_buffer = {
  async test(ctrl, env, ctx) {
    class StreamWritable extends Writable {
      constructor() {
        super({
          objectMode: true
        });
      }

      // Refs: https://github.com/nodejs/node/issues/6758
      // We need a timer like on the original issue thread.
      // Otherwise the code will never reach our test case.
      _write(chunk, encoding, cb) {
        queueMicrotask(cb);
      }
    }
    const testStream = new StreamWritable();
    testStream.cork();
    const writePromises = [];
    for (let i = 1; i <= 5; i++) {
      const p = deferredPromise();
      writePromises.push(p.promise);
      testStream.write(
        i,
        () => {
          strictEqual(testStream._writableState.bufferedRequestCount, testStream._writableState.getBuffer().length);
          p.resolve();
        }
      );
    }
    testStream.end()
    await Promise.all(writePromises);
  }
};

export const writable_change_deafult_encoding = {
  async test(ctrl, env, ctx) {
    class MyWritable extends Writable {
      constructor(fn, options) {
        super(options);
        this.fn = fn;
      }
      _write(chunk, encoding, callback) {
        this.fn(Buffer.isBuffer(chunk), typeof chunk, encoding);
        callback();
      }
    }

    await (async function defaultCondingIsUtf8() {
      const m = new MyWritable(
        function (isBuffer, type, enc) {
          strictEqual(enc, 'utf8');
        },
        {
          decodeStrings: false
        }
      )
      m.write('foo');
      m.end();
      await promises.finished(m);
    })();

    await (async function changeDefaultEncodingToAscii() {
      const m = new MyWritable(
        function (isBuffer, type, enc) {
          strictEqual(enc, 'ascii');
        },
        {
          decodeStrings: false
        }
      );
      m.setDefaultEncoding('ascii');
      m.write('bar');
      m.end();
      await promises.finished(m);
    })()

    // Change default encoding to invalid value.
    throws(
      () => {
        const m = new MyWritable((isBuffer, type, enc) => {}, {
          decodeStrings: false
        })
        m.setDefaultEncoding({});
        m.write('bar');
        m.end();
      },
      {
        code: 'ERR_UNKNOWN_ENCODING',
      }
    );
    await (async function checkVariableCaseEncoding() {
      const m = new MyWritable(
        function (isBuffer, type, enc) {
          strictEqual(enc, 'ascii');
        },
        {
          decodeStrings: false
        }
      )
      m.setDefaultEncoding('AsCii');
      m.write('bar');
      m.end();
      await promises.finished(m);
    })()
  }
};

export const writable_aborted = {
  async test(ctrl, env, ctx) {
    {
      const writable = new Writable({
        write() {}
      });
      strictEqual(writable.writableAborted, false);
      writable.destroy();
      strictEqual(writable.writableAborted, true);
    }
    {
      const writable = new Writable({
        write() {}
      });
      strictEqual(writable.writableAborted, false);
      writable.end();
      writable.destroy();
      strictEqual(writable.writableAborted, true);
    }
  }
};

export const unshift_read_race = {
  async test(ctrl, env, ctx) {
    const hwm = 10;
    const r = Readable({
      highWaterMark: hwm,
      autoDestroy: false
    });
    const chunks = 10;
    const data = Buffer.allocUnsafe(chunks * hwm + Math.ceil(hwm / 2));
    for (let i = 0; i < data.length; i++) {
      const c = 'asdf'.charCodeAt(i % 4);
      data[i] = c;
    }
    let pos = 0;
    let pushedNull = false;
    r._read = function (n) {
      ok(!pushedNull, '_read after null push');

      // Every third chunk is fast
      push(!(chunks % 3));
      function push(fast) {
        ok(!pushedNull, 'push() after null push');
        const c = pos >= data.length ? null : data.slice(pos, pos + n);
        pushedNull = c === null;
        if (fast) {
          pos += n;
          r.push(c);
          if (c === null) pushError();
        } else {
          setTimeout(function () {
            pos += n;
            r.push(c);
            if (c === null) pushError();
          }, 1);
        }
      }
    }
    function pushError() {
      r.unshift(Buffer.allocUnsafe(1));
      w.end();
      throws(
        () => {
          r.push(Buffer.allocUnsafe(1));
        },
        {
          code: 'ERR_STREAM_PUSH_AFTER_EOF',
          name: 'Error',
          message: 'stream.push() after EOF'
        }
      );
    }
    const w = Writable();
    const written = [];
    const finishCalled = deferredPromise();
    w._write = function (chunk, encoding, cb) {
      written.push(chunk.toString());
      cb();
    }
    r.on('end', finishCalled.reject);
    r.on('readable', function () {
      let chunk;
      while (null !== (chunk = r.read(10))) {
        w.write(chunk);
        if (chunk.length > 4) r.unshift(Buffer.from('1234'));
      }
    });
    w.on(
      'finish',
      function () {
        // Each chunk should start with 1234, and then be asfdasdfasdf...
        // The first got pulled out before the first unshift('1234'), so it's
        // lacking that piece.
        strictEqual(written[0], 'asdfasdfas');
        let asdf = 'd';
        for (let i = 1; i < written.length; i++) {
          strictEqual(written[i].slice(0, 4), '1234');
          for (let j = 4; j < written[i].length; j++) {
            const c = written[i].charAt(j);
            strictEqual(c, asdf);
            switch (asdf) {
              case 'a':
                asdf = 's';
                break;
              case 's':
                asdf = 'd';
                break;
              case 'd':
                asdf = 'f';
                break;
              case 'f':
                asdf = 'a';
                break;
            }
          }
        }
        finishCalled.resolve();
      }
    );
    await finishCalled.promise;
    strictEqual(written.length, 18);
  }
};

export const unshift_empty_chunk = {
  async test(ctrl, env, ctx) {
    const r = new Readable();
    let nChunks = 10;
    const chunk = Buffer.alloc(10, 'x');
    r._read = function (n) {
      queueMicrotask(() => {
        r.push(--nChunks === 0 ? null : chunk)
      });
    }
    let readAll = false;
    const seen = [];
    r.on('readable', () => {
      let chunk;
      while ((chunk = r.read()) !== null) {
        seen.push(chunk.toString());
        // Simulate only reading a certain amount of the data,
        // and then putting the rest of the chunk back into the
        // stream, like a parser might do.  We just fill it with
        // 'y' so that it's easy to see which bits were touched,
        // and which were not.
        const putBack = Buffer.alloc(readAll ? 0 : 5, 'y');
        readAll = !readAll;
        r.unshift(putBack);
      }
    })
    const expect = [
      'xxxxxxxxxx',
      'yyyyy',
      'xxxxxxxxxx',
      'yyyyy',
      'xxxxxxxxxx',
      'yyyyy',
      'xxxxxxxxxx',
      'yyyyy',
      'xxxxxxxxxx',
      'yyyyy',
      'xxxxxxxxxx',
      'yyyyy',
      'xxxxxxxxxx',
      'yyyyy',
      'xxxxxxxxxx',
      'yyyyy',
      'xxxxxxxxxx',
      'yyyyy'
    ];
    r.on('end', () => {
      deepStrictEqual(seen, expect);
    });
    await promises.finished(r);
  }
};

export const unpipe_event = {
  async test(ctrl, env, ctx) {
    class NullWriteable extends Writable {
      _write(chunk, encoding, callback) {
        return callback();
      }
    }
    class QuickEndReadable extends Readable {
      _read() {
        this.push(null);
      }
    }
    class NeverEndReadable extends Readable {
      _read() {}
    }
    {
      const pipeCalled = deferredPromise();
      const unpipeCalled = deferredPromise();
      const dest = new NullWriteable();
      const src = new QuickEndReadable();
      dest.on('pipe', pipeCalled.resolve);
      dest.on('unpipe', unpipeCalled.resolve);
      src.pipe(dest);
      await Promise.all([
        pipeCalled.promise,
        unpipeCalled.promise,
      ]);
      strictEqual(src._readableState.pipes.length, 0);
    }
    {
      const pipeCalled = deferredPromise();
      const dest = new NullWriteable();
      const src = new NeverEndReadable();
      dest.on('pipe', pipeCalled.resolve);
      dest.on('unpipe', pipeCalled.reject);
      src.pipe(dest);
      await pipeCalled.promise;
      strictEqual(src._readableState.pipes.length, 1);
    }
    {
      const pipeCalled = deferredPromise();
      const unpipeCalled = deferredPromise();
      const dest = new NullWriteable();
      const src = new NeverEndReadable();
      dest.on('pipe', pipeCalled.resolve);
      dest.on('unpipe', unpipeCalled.resolve);
      src.pipe(dest);
      src.unpipe(dest);
      await Promise.all([
        pipeCalled.promise,
        unpipeCalled.promise,
      ]);
      strictEqual(src._readableState.pipes.length, 0);
    }
    {
      const pipeCalled = deferredPromise();
      const unpipeCalled = deferredPromise();
      const dest = new NullWriteable();
      const src = new QuickEndReadable();
      dest.on('pipe', pipeCalled.resolve);
      dest.on('unpipe', unpipeCalled.resolve);
      src.pipe(dest, {
        end: false
      })
      await Promise.all([
        pipeCalled.promise,
        unpipeCalled.promise,
      ]);
      strictEqual(src._readableState.pipes.length, 0);
    }
    {
      const pipeCalled = deferredPromise();
      const dest = new NullWriteable()
      const src = new NeverEndReadable()
      dest.on('pipe', pipeCalled.resolve)
      dest.on('unpipe', pipeCalled.reject)
      src.pipe(dest, {
        end: false
      })
      await pipeCalled.promise;
      strictEqual(src._readableState.pipes.length, 1);
    }
    {
      const pipeCalled = deferredPromise();
      const unpipeCalled = deferredPromise();
      const dest = new NullWriteable();
      const src = new NeverEndReadable();
      dest.on('pipe', pipeCalled.resolve);
      dest.on('unpipe', unpipeCalled.resolve);
      src.pipe(dest, {
        end: false
      });
      src.unpipe(dest);
      await Promise.all([
        pipeCalled.promise,
        unpipeCalled.promise,
      ]);
      strictEqual(src._readableState.pipes.length, 0);
    }
  }
};

export const uint8array = {
  async test(ctrl, env, ctx) {
    const ABC = new Uint8Array([0x41, 0x42, 0x43]);
    const DEF = new Uint8Array([0x44, 0x45, 0x46]);
    const GHI = new Uint8Array([0x47, 0x48, 0x49]);
    {
      // Simple Writable test.

      let n = 0;
      const writeCalled = deferredPromise();
      let writeCount = 0;
      const writable = new Writable({
        write: (chunk, encoding, cb) => {
          ok(chunk instanceof Buffer);
          if (n++ === 0) {
            strictEqual(String(chunk), 'ABC');
          } else {
            strictEqual(String(chunk), 'DEF');
          }
          cb();
          if (++writeCount === 2) writeCalled.resolve();
        }
      });
      writable.write(ABC);
      writable.end(DEF);
      await writeCalled.promise;
    }
    {
      // Writable test, pass in Uint8Array in object mode.
      const writeCalled = deferredPromise();
      const writable = new Writable({
        objectMode: true,
        write: (chunk, encoding, cb) => {
          ok(!(chunk instanceof Buffer));
          ok(chunk instanceof Uint8Array);
          strictEqual(chunk, ABC);
          strictEqual(encoding, 'utf8');
          cb();
          writeCalled.resolve();
        }
      });
      writable.end(ABC);
      await writeCalled.promise;
    }
    {
      // Writable test, multiple writes carried out via writev.
      let callback;
      const writeCalled = deferredPromise();
      const writevCalled = deferredPromise();
      const writable = new Writable({
        write: (chunk, encoding, cb) => {
          ok(chunk instanceof Buffer);
          strictEqual(encoding, 'buffer');
          strictEqual(String(chunk), 'ABC');
          callback = cb;
          writeCalled.resolve();
        },
        writev: (chunks, cb) => {
          strictEqual(chunks.length, 2);
          strictEqual(chunks[0].encoding, 'buffer');
          strictEqual(chunks[1].encoding, 'buffer');
          strictEqual(chunks[0].chunk + chunks[1].chunk, 'DEFGHI');
          writevCalled.resolve();
        }
      });
      writable.write(ABC);
      writable.write(DEF);
      writable.end(GHI);
      callback();
      await Promise.all([
        writeCalled.promise,
        writevCalled.promise,
      ]);
    }
    {
      // Simple Readable test.
      const readable = new Readable({
        read() {}
      });
      readable.push(DEF);
      readable.unshift(ABC);
      const buf = readable.read();
      ok(buf instanceof Buffer);
      deepStrictEqual([...buf], [...ABC, ...DEF]);
    }
    {
      // Readable test, setEncoding.
      const readable = new Readable({
        read() {}
      });
      readable.setEncoding('utf8');
      readable.push(DEF);
      readable.unshift(ABC);
      const out = readable.read();
      strictEqual(out, 'ABCDEF');
    }
  }
};

export const transform_split_objectmode = {
  async test(ctrl, env, ctx) {
    const parser = new Transform({
      readableObjectMode: true
    });
    ok(parser._readableState.objectMode);
    ok(!parser._writableState.objectMode);
    strictEqual(parser.readableHighWaterMark, 16);
    strictEqual(parser.writableHighWaterMark, 16 * 1024);
    strictEqual(parser.readableHighWaterMark, parser._readableState.highWaterMark);
    strictEqual(parser.writableHighWaterMark, parser._writableState.highWaterMark);
    parser._transform = function (chunk, enc, callback) {
      callback(null, {
        val: chunk[0]
      });
    };
    let parsed;
    parser.on('data', function (obj) {
      parsed = obj;
    });
    parser.end(Buffer.from([42]));

    const serializer = new Transform({
      writableObjectMode: true
    });
    ok(!serializer._readableState.objectMode);
    ok(serializer._writableState.objectMode);
    strictEqual(serializer.readableHighWaterMark, 16 * 1024);
    strictEqual(serializer.writableHighWaterMark, 16);
    strictEqual(parser.readableHighWaterMark, parser._readableState.highWaterMark);
    strictEqual(parser.writableHighWaterMark, parser._writableState.highWaterMark);
    serializer._transform = function (obj, _, callback) {
      callback(null, Buffer.from([obj.val]));
    };
    let serialized;
    serializer.on('data', function (chunk) {
      serialized = chunk;
    });
    serializer.write({
      val: 42
    });

    strictEqual(parsed.val, 42);
    strictEqual(serialized[0], 42);
  }
};

export const transform_split_highwatermark = {
  async test(ctrl, env, ctx) {
    const DEFAULT = 16 * 1024;
    function testTransform(expectedReadableHwm, expectedWritableHwm, options) {
      const t = new Transform(options);
      strictEqual(t._readableState.highWaterMark, expectedReadableHwm);
      strictEqual(t._writableState.highWaterMark, expectedWritableHwm);
    }

    // Test overriding defaultHwm
    testTransform(666, DEFAULT, {
      readableHighWaterMark: 666
    });
    testTransform(DEFAULT, 777, {
      writableHighWaterMark: 777
    });
    testTransform(666, 777, {
      readableHighWaterMark: 666,
      writableHighWaterMark: 777
    });

    // Test highWaterMark overriding
    testTransform(555, 555, {
      highWaterMark: 555,
      readableHighWaterMark: 666
    });
    testTransform(555, 555, {
      highWaterMark: 555,
      writableHighWaterMark: 777
    });
    testTransform(555, 555, {
      highWaterMark: 555,
      readableHighWaterMark: 666,
      writableHighWaterMark: 777
    });

    // Test undefined, null
    ;[undefined, null].forEach((v) => {
      testTransform(DEFAULT, DEFAULT, {
        readableHighWaterMark: v
      });
      testTransform(DEFAULT, DEFAULT, {
        writableHighWaterMark: v
      });
      testTransform(666, DEFAULT, {
        highWaterMark: v,
        readableHighWaterMark: 666
      });
      testTransform(DEFAULT, 777, {
        highWaterMark: v,
        writableHighWaterMark: 777
      });
    });

    // test NaN
    {
      throws(
        () => {
          new Transform({
            readableHighWaterMark: NaN
          });
        },
        {
          name: 'TypeError',
          code: 'ERR_INVALID_ARG_VALUE',
        }
      );
      throws(
        () => {
          new Transform({
            writableHighWaterMark: NaN
          });
        },
        {
          name: 'TypeError',
          code: 'ERR_INVALID_ARG_VALUE',
        }
      );
    }

    // Test non Duplex streams ignore the options
    {
      const r = new Readable({
        readableHighWaterMark: 666
      });
      strictEqual(r._readableState.highWaterMark, DEFAULT);
      const w = new Writable({
        writableHighWaterMark: 777
      });
      strictEqual(w._writableState.highWaterMark, DEFAULT);
    }
  }
};

export const transform_objectmode_falsey_value = {
  async test(ctrl, env, ctx) {
    const src = new PassThrough({
      objectMode: true
    });
    const tx = new PassThrough({
      objectMode: true
    });
    const dest = new PassThrough({
      objectMode: true
    });
    const expect = [-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
    const results = [];
    const dataCalled = deferredPromise();
    const intervalFinished = deferredPromise();
    let dataCount = 0;
    let intCount = 0;
    dest.on(
      'data',
      function (x) {
        results.push(x);
        if (++dataCount === expect.length) dataCalled.resolve();
      }
    )
    src.pipe(tx).pipe(dest);
    let i = -1;
    const int = setInterval(
      function () {
        if (results.length === expect.length) {
          src.end();
          clearInterval(int);
          deepStrictEqual(results, expect);
        } else {
          src.write(i++);
        }
        if (++intCount === expect.length + 1) intervalFinished.resolve();
      }, 1);
    await Promise.all([
      dataCalled.promise,
      intervalFinished.promise,
    ]);
  }
};

export const transform_hwm0 = {
  async test(ctrl, env, ctx) {
    const t = new Transform({
      objectMode: true,
      highWaterMark: 0,
      transform(chunk, enc, callback) {
        queueMicrotask(() => callback(null, chunk, enc));
      }
    });
    strictEqual(t.write(1), false);
    const drainCalled = deferredPromise();
    const readableCalled = deferredPromise();
    t.on(
      'drain',
      () => {
        strictEqual(t.write(2), false);
        t.end();
        drainCalled.resolve();
      }
    )
    t.once(
      'readable',
      () => {
        strictEqual(t.read(), 1);
        queueMicrotask(
          () => {
            strictEqual(t.read(), null);
            t.once(
              'readable',
              () => {
                strictEqual(t.read(), 2);
                readableCalled.resolve();
              }
            );
          }
        );
      }
    );
    await Promise.all([
      drainCalled.promise,
      readableCalled.promise,
    ]);
  }
};

export const transform_flush_data = {
  async test(ctrl, env, ctx) {
    const expected = 'asdf';
    function _transform(d, e, n) {
      n();
    }
    function _flush(n) {
      n(null, expected);
    }
    const t = new Transform({
      transform: _transform,
      flush: _flush
    });
    t.end(Buffer.from('blerg'));
    t.on('data', (data) => {
      strictEqual(data.toString(), expected);
    });
    await promises.finished(t);
  }
};

export const transform_final = {
  async test(ctrl, env, ctx) {
    let state = 0;
    const transformCalled = deferredPromise();
    const finalCalled = deferredPromise();
    const flushCalled = deferredPromise();
    const finishCalled = deferredPromise();
    const endCalled = deferredPromise();
    const dataCalled =deferredPromise();
    const endFinished = deferredPromise();
    let dataCount = 0;
    let transformCount = 0;
    const t = new Transform({
      objectMode: true,
      transform: function (chunk, _, next) {
        // transformCallback part 1
        strictEqual(++state, chunk);
        this.push(state);
        // transformCallback part 2
        strictEqual(++state, chunk + 2);
        queueMicrotask(next);
        if (++transformCount === 3) transformCalled.resolve();
      },
      final: function (done) {
        state++;
        // finalCallback part 1
        strictEqual(state, 10);
        setTimeout(function () {
          state++;
          // finalCallback part 2
          strictEqual(state, 11);
          done();
          finalCalled.resolve();
        }, 100);
      },
      flush: function (done) {
        state++;
        // flushCallback part 1
        strictEqual(state, 12);
        queueMicrotask(function () {
          state++;
          // flushCallback part 2
          strictEqual(state, 13);
          done();
          flushCalled.resolve();
        })
      }
    });
    t.on(
      'finish',
      function () {
        state++;
        // finishListener
        strictEqual(state, 15);
        finishCalled.resolve();
      }
    );
    t.on(
      'end',
      function () {
        state++;
        // end event
        strictEqual(state, 16);
        endCalled.resolve();
      }
    );
    t.on(
      'data',
      function (d) {
        // dataListener
        strictEqual(++state, d + 1);
        if (++dataCount) dataCalled.resolve();
      }
    );
    t.write(1);
    t.write(4);
    t.end(
      7,
      function () {
        state++;
        // endMethodCallback
        strictEqual(state, 14);
        endFinished.resolve();
      }
    );
    await Promise.all([
      transformCalled.promise,
      finalCalled.promise,
      flushCalled.promise,
      finishCalled.promise,
      endCalled.promise,
      dataCalled.promise,
      endFinished.promise,
    ]);
  }
};

export const transform_final_sync = {
  async test(ctrl, env, ctx) {
    let state = 0;
    const transformCalled = deferredPromise();
    const finalCalled = deferredPromise();
    const flushCalled = deferredPromise();
    const finishCalled = deferredPromise();
    const endCalled = deferredPromise();
    const dataCalled = deferredPromise();
    const endFinished = deferredPromise();
    let transformCount = 0;
    let dataCount = 0;
    const t = new Transform({
      objectMode: true,
      transform: function (chunk, _, next) {
        // transformCallback part 1
        strictEqual(++state, chunk);
        this.push(state);
        // transformCallback part 2
        strictEqual(++state, chunk + 2);
        queueMicrotask(next);
        if (++transformCount === 3) transformCalled.resolve();
      },
      final: function (done) {
        state++;
        // finalCallback part 1
        strictEqual(state, 10);
        state++;
        // finalCallback part 2
        strictEqual(state, 11);
        done();
        finalCalled.resolve();
      },
      flush: function (done) {
        state++;
        // fluchCallback part 1
        strictEqual(state, 12);
        queueMicrotask(function () {
          state++;
          // fluchCallback part 2
          strictEqual(state, 13);
          done();
          flushCalled.resolve();
        });
      }
    });
    t.on(
      'finish',
      function () {
        state++;
        // finishListener
        strictEqual(state, 15);
        finishCalled.resolve();
      }
    );
    t.on(
      'end',
      function () {
        state++;
        // endEvent
        strictEqual(state, 16);
        endCalled.resolve();
      }
    );
    t.on(
      'data',
      function (d) {
        // dataListener
        strictEqual(++state, d + 1);
        if (++dataCount === 3) dataCalled.resolve();
      }
    );
    t.write(1);
    t.write(4);
    t.end(
      7,
      function () {
        state++;
        // endMethodCallback
        strictEqual(state, 14);
        endFinished.resolve();
      }
    );
    await Promise.all([
      transformCalled.promise,
      finalCalled.promise,
      flushCalled.promise,
      finishCalled.promise,
      endCalled.promise,
      dataCalled.promise,
      endFinished.promise,
    ]);
  }
};

export const transform_destroy = {
  async test(ctrl, env, ctx) {
    {
      const transform = new Transform({
        transform(chunk, enc, cb) {}
      });
      transform.resume();
      const closed = deferredPromise();
      transform.on('end', closed.reject);
      transform.on('close', closed.resolve);
      transform.on('finish', closed.reject);
      transform.destroy();
      await closed.promise;
    }
    {
      const transform = new Transform({
        transform(chunk, enc, cb) {}
      });
      transform.resume();
      const expected = new Error('kaboom');
      const errored = deferredPromise();
      const closed = deferredPromise();
      transform.on('end', closed.reject);
      transform.on('finish', closed.reject);
      transform.on('close', closed.resolve);
      transform.on(
        'error',
        (err) => {
          strictEqual(err, expected);
          errored.resolve();
        }
      );
      transform.destroy(expected);
      await Promise.all([
        closed.promise,
        errored.promise,
      ]);
    }
    {
      const transform = new Transform({
        transform(chunk, enc, cb) {}
      });
      transform._destroy = function (err, cb) {
        strictEqual(err, expected);
        cb(err);
      };
      const expected = new Error('kaboom');
      const closed = deferredPromise();
      const errored = deferredPromise();
      transform.on('finish', closed.reject);
      transform.on('close', closed.resolve);
      transform.on(
        'error',
        (err) => {
          strictEqual(err, expected);
          errored.resolve();
        }
      )
      transform.destroy(expected);
      await Promise.all([
        closed.promise,
        errored.promise,
      ]);
    }
    {
      const expected = new Error('kaboom');
      const destroyCalled = deferredPromise();
      const closed = deferredPromise();
      const transform = new Transform({
        transform(chunk, enc, cb) {},
        destroy: function (err, cb) {
          strictEqual(err, expected);
          cb();
          destroyCalled.resolve();
        }
      });
      transform.resume();
      transform.on('end', closed.reject);
      transform.on('close', closed.resolve);
      transform.on('finish', closed.reject);

      // Error is swallowed by the custom _destroy
      transform.on('error', closed.reject);
      transform.destroy(expected);
      await Promise.all([
        destroyCalled.promise,
        closed.promise,
      ]);
    }
    {
      const transform = new Transform({
        transform(chunk, enc, cb) {}
      });
      const destroyCalled = deferredPromise();
      transform._destroy = function (err, cb) {
        strictEqual(err, null);
        cb();
        destroyCalled.resolve();
      };
      transform.destroy();
      await destroyCalled.promise;
    }
    {
      const transform = new Transform({
        transform(chunk, enc, cb) {}
      });
      transform.resume();
      const destroyCalled = deferredPromise();
      const closed = deferredPromise();
      const endCalled = deferredPromise();
      transform._destroy = function (err, cb) {
        strictEqual(err, null);
        queueMicrotask(() => {
          this.push(null);
          this.end();
          cb();
          destroyCalled.resolve();
        })
      };
      transform.on('finish', closed.reject);
      transform.on('end', closed.reject);
      transform.on('close', closed.resolve);
      transform.destroy();
      transform.removeListener('end', closed.reject);
      transform.removeListener('finish', closed.reject);
      transform.on('end', endCalled.resolve);
      transform.on('finish', closed.reject);
      await Promise.all([
        destroyCalled.promise,
        closed.promise,
        endCalled.promise,
      ]);
    }
    {
      const transform = new Transform({
        transform(chunk, enc, cb) {}
      });
      const expected = new Error('kaboom');
      const destroyCalled = deferredPromise();
      const closed = deferredPromise();
      const errored = deferredPromise();
      transform._destroy = function (err, cb) {
        strictEqual(err, null);
        cb(expected);
        destroyCalled.resolve();
      };
      transform.on('close', closed.resolve);
      transform.on('finish', closed.reject);
      transform.on('end', closed.reject);
      transform.on(
        'error',
        (err) => {
          strictEqual(err, expected);
          errored.resolve();
        }
      );
      transform.destroy();
      await Promise.all([
        destroyCalled.promise,
        closed.promise,
        errored.promise,
      ]);
    }
  }
};

export const transform_constructor_set_methods = {
  async test(ctrl, env, ctx) {
    const t = new Transform();
    throws(
      () => {
        t.end(Buffer.from('blerg'));
      },
      {
        name: 'Error',
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
        message: 'The _transform() method is not implemented'
      }
    );
    const transformCalled = deferredPromise();
    const finalCalled = deferredPromise();
    const flushCalled = deferredPromise();
    const _transform = (chunk, _, next) => {
      next();
      transformCalled.resolve();
    };
    const _final = (next) => {
      next();
      finalCalled.resolve();
    };
    const _flush = (next) => {
      next();
      flushCalled.resolve();
    };
    const t2 = new Transform({
      transform: _transform,
      flush: _flush,
      final: _final
    });
    strictEqual(t2._transform, _transform);
    strictEqual(t2._flush, _flush);
    strictEqual(t2._final, _final);
    t2.end(Buffer.from('blerg'));
    t2.resume();
    await Promise.all([
      transformCalled.promise,
      finalCalled.promise,
      flushCalled.promise,
    ]);
  }
};

export const transform_callback_twice = {
  async test(ctrl, env, ctx) {
    const stream = new Transform({
      transform(chunk, enc, cb) {
        cb();
        cb();
      }
    });
    const errored = deferredPromise();
    stream.on(
      'error',
      function (err) {
        strictEqual(err.code, 'ERR_MULTIPLE_CALLBACK');
        errored.resolve();
      }
    )
    stream.write('foo');
    await errored.promise;
  }
};

export const toarray = {
  async test(ctrl, env, ctx) {
    {
      // Works on a synchronous stream
      await (async () => {
        const tests = [
          [],
          [1],
          [1, 2, 3],
          Array(100)
            .fill()
            .map((_, i) => i)
        ];
        for (const test of tests) {
          const stream = Readable.from(test);
          const result = await stream.toArray();
          deepStrictEqual(result, test);
        }
      })();
    }
    {
      // Works on a non-object-mode stream
      await (async () => {
        const firstBuffer = Buffer.from([1, 2, 3]);
        const secondBuffer = Buffer.from([4, 5, 6]);
        const stream = Readable.from([firstBuffer, secondBuffer], {
          objectMode: false
        });
        const result = await stream.toArray();
        strictEqual(Array.isArray(result), true);
        deepStrictEqual(result, [firstBuffer, secondBuffer]);
      })();
    }
    {
      // Works on an asynchronous stream
      await (async () => {
        const tests = [
          [],
          [1],
          [1, 2, 3],
          Array(100)
            .fill()
            .map((_, i) => i)
        ];
        for (const test of tests) {
          const stream = Readable.from(test).map((x) => Promise.resolve(x));
          const result = await stream.toArray();
          deepStrictEqual(result, test);
        }
      })();
    }
    {
      // Support for AbortSignal
      const ac = new AbortController();
      let stream;
      queueMicrotask(() => ac.abort());
      await rejects(
          async () => {
            stream = Readable.from([1, 2, 3]).map(async (x) => {
              if (x === 3) {
                await new Promise(() => {}); // Explicitly do not pass signal here
              }

              return Promise.resolve(x);
            })
            await stream.toArray({
              signal: ac.signal
            });
          },
          {
            name: 'AbortError'
          }
        );

      // Only stops toArray, does not destroy the stream
      ok(stream.destroyed, false);
    }
    {
      // Test result is a Promise
      const result = Readable.from([1, 2, 3, 4, 5]).toArray();
      strictEqual(result instanceof Promise, true);
    }
    {
      // Error cases
      await rejects(async () => {
          await Readable.from([1]).toArray(1)
        }, /ERR_INVALID_ARG_TYPE/);
      await rejects(async () => {
          await Readable.from([1]).toArray({
            signal: true
          })
        }, /ERR_INVALID_ARG_TYPE/);
    }
  }
};

export const some_find_every = {
  async test(ctrl, env, ctx) {
    function oneTo5() {
      return Readable.from([1, 2, 3, 4, 5]);
    }

    function oneTo5Async() {
      return oneTo5().map(async (x) => {
        await Promise.resolve();
        return x;
      });
    }
    {
      // Some, find, and every work with a synchronous stream and predicate
      strictEqual(await oneTo5().some((x) => x > 3), true);
      strictEqual(await oneTo5().every((x) => x > 3), false);
      strictEqual(await oneTo5().find((x) => x > 3), 4);
      strictEqual(await oneTo5().some((x) => x > 6), false);
      strictEqual(await oneTo5().every((x) => x < 6), true);
      strictEqual(await oneTo5().find((x) => x > 6), undefined);
      strictEqual(await Readable.from([]).some(() => true), false);
      strictEqual(await Readable.from([]).every(() => true), true);
      strictEqual(await Readable.from([]).find(() => true), undefined);
    }

    {
      // Some, find, and every work with an asynchronous stream and synchronous predicate
      strictEqual(await oneTo5Async().some((x) => x > 3), true);
      strictEqual(await oneTo5Async().every((x) => x > 3), false);
      strictEqual(await oneTo5Async().find((x) => x > 3), 4);
      strictEqual(await oneTo5Async().some((x) => x > 6), false);
      strictEqual(await oneTo5Async().every((x) => x < 6), true);
      strictEqual(await oneTo5Async().find((x) => x > 6), undefined);
    }

    {
      // Some, find, and every work on synchronous streams with an asynchronous predicate
      strictEqual(await oneTo5().some(async (x) => x > 3), true);
      strictEqual(await oneTo5().every(async (x) => x > 3), false);
      strictEqual(await oneTo5().find(async (x) => x > 3), 4);
      strictEqual(await oneTo5().some(async (x) => x > 6), false);
      strictEqual(await oneTo5().every(async (x) => x < 6), true);
      strictEqual(await oneTo5().find(async (x) => x > 6), undefined);
    }

    {
      // Some, find, and every work on asynchronous streams with an asynchronous predicate
      strictEqual(await oneTo5Async().some(async (x) => x > 3), true);
      strictEqual(await oneTo5Async().every(async (x) => x > 3), false);
      strictEqual(await oneTo5Async().find(async (x) => x > 3), 4);
      strictEqual(await oneTo5Async().some(async (x) => x > 6), false);
      strictEqual(await oneTo5Async().every(async (x) => x < 6), true);
      strictEqual(await oneTo5Async().find(async (x) => x > 6), undefined);
    }

    {
      async function checkDestroyed(stream) {
        await scheduler.wait(1);
        strictEqual(stream.destroyed, true);
      }

      {
        // Some, find, and every short circuit
        const someStream = oneTo5();
        await someStream.some((x) => x > 2);
        await checkDestroyed(someStream);

        const everyStream = oneTo5();
        await everyStream.every((x) => x < 3);
        await checkDestroyed(everyStream);

        const findStream = oneTo5();
        await findStream.find((x) => x > 1);
        await checkDestroyed(findStream);

        // When short circuit isn't possible the whole stream is iterated
        await oneTo5().some(() => false);
        await oneTo5().every(() => true);
        await oneTo5().find(() => false);
      }

      {
        // Some, find, and every short circuit async stream/predicate
        const someStream = oneTo5Async();
        await someStream.some(async (x) => x > 2);
        await checkDestroyed(someStream);

        const everyStream = oneTo5Async();
        await everyStream.every(async (x) => x < 3);
        await checkDestroyed(everyStream);

        const findStream = oneTo5Async();
        await findStream.find(async (x) => x > 1);
        await checkDestroyed(findStream);

        // When short circuit isn't possible the whole stream is iterated
        await oneTo5Async().some(async () => false);
        await oneTo5Async().every(async () => true);
        await oneTo5Async().find(async () => false);
      }
    }

    {
      // Concurrency doesn't affect which value is found.
      const found = await Readable.from([1, 2]).find(
        async (val) => {
          if (val === 1) {
            await scheduler.wait(100);
          }
          return true;
        },
        { concurrency: 2 }
      )
      strictEqual(found, 1);
    }

    {
      // Support for AbortSignal
      for (const op of ['some', 'every', 'find']) {
        {
          const ac = new AbortController();
          queueMicrotask(() => ac.abort());
          await rejects(
              Readable.from([1, 2, 3])[op](() => new Promise(() => {}), { signal: ac.signal }),
              {
                name: 'AbortError'
              },
              `${op} should abort correctly with sync abort`
            );
        }
        {
          // Support for pre-aborted AbortSignal
          await rejects(
              Readable.from([1, 2, 3])[op](() => new Promise(() => {}), { signal: AbortSignal.abort() }),
              {
                name: 'AbortError'
              },
              `${op} should abort with pre-aborted abort controller`
            );
        }
      }
    }
    {
      // Error cases
      for (const op of ['some', 'every', 'find']) {
        await rejects(
            async () => {
              await Readable.from([1])[op](1);
            },
            /ERR_INVALID_ARG_TYPE/,
            `${op} should throw for invalid function`
          );
        await rejects(
            async () => {
              await Readable.from([1])[op]((x) => x, {
                concurrency: 'Foo'
              });
            },
            /RangeError/,
            `${op} should throw for invalid concurrency`
          );
        await rejects(
            async () => {
              await Readable.from([1])[op]((x) => x, 1);
            },
            /ERR_INVALID_ARG_TYPE/,
            `${op} should throw for invalid concurrency`
          );
        await rejects(
            async () => {
              await Readable.from([1])[op]((x) => x, {
                signal: true
              });
            },
            /ERR_INVALID_ARG_TYPE/,
            `${op} should throw for invalid signal`
          );
      }
    }
    {
      for (const op of ['some', 'every', 'find']) {
        const stream = oneTo5();
        Object.defineProperty(stream, 'map', {
          value: () => {
            throw new Error('should not be called');
          }
        });
        // Check that map isn't getting called.
        stream[op](() => {});
      }
    }
  }
};

export const reduce = {
  async test(ctrl, env, ctx) {
    function sum(p, c) {
      return p + c;
    }
    {
      // Does the same thing as `(await stream.toArray()).reduce(...)`
      await (async () => {
        const tests = [
          [[], sum, 0],
          [[1], sum, 0],
          [[1, 2, 3, 4, 5], sum, 0],
          [[...Array(100).keys()], sum, 0],
          [['a', 'b', 'c'], sum, ''],
          [[1, 2], sum],
          [[1, 2, 3], (x, y) => y]
        ];
        for (const [values, fn, initial] of tests) {
          const streamReduce = await Readable.from(values).reduce(fn, initial);
          const arrayReduce = values.reduce(fn, initial);
          deepStrictEqual(streamReduce, arrayReduce);
        }
        // Does the same thing as `(await stream.toArray()).reduce(...)` with an
        // asynchronous reducer
        for (const [values, fn, initial] of tests) {
          const streamReduce = await Readable.from(values)
            .map(async (x) => x)
            .reduce(fn, initial);
          const arrayReduce = values.reduce(fn, initial);
          deepStrictEqual(streamReduce, arrayReduce);
        }
      })();
    }
    {
      // Works with an async reducer, with or without initial value
      await (async () => {
        const six = await Readable.from([1, 2, 3]).reduce(async (p, c) => p + c, 0);
        strictEqual(six, 6);
      })();
      await (async () => {
        const six = await Readable.from([1, 2, 3]).reduce(async (p, c) => p + c);
        strictEqual(six, 6);
      })();
    }
    {
      // Works lazily
      await rejects(
          Readable.from([1, 2, 3, 4, 5, 6])
            .map(
              (x) => {
                return x
              }
            ) // Two consumed and one buffered by `map` due to default concurrency
            .reduce(async (p, c) => {
              if (p === 1) {
                throw new Error('boom');
              }
              return c;
            }, 0),
          /boom/
        );
    }
    {
      // Support for AbortSignal
      const ac = new AbortController();
      queueMicrotask(() => ac.abort());
      await rejects(
          async () => {
            await Readable.from([1, 2, 3]).reduce(
              async (p, c) => {
                if (c === 3) {
                  await new Promise(() => {}); // Explicitly do not pass signal here
                }

                return Promise.resolve();
              },
              0,
              {
                signal: ac.signal
              }
            );
          },
          {
            name: 'AbortError'
          }
        );
    }
    {
      // Support for AbortSignal - pre aborted
      const stream = Readable.from([1, 2, 3]);
      await rejects(
          async () => {
            await stream.reduce(
              async (p, c) => {
                if (c === 3) {
                  await new Promise(() => {}) // Explicitly do not pass signal here
                }

                return Promise.resolve()
              },
              0,
              {
                signal: AbortSignal.abort()
              }
            )
          },
          {
            name: 'AbortError'
          }
        );

      strictEqual(stream.destroyed, true);
    }
    {
      // Support for AbortSignal - deep
      const stream = Readable.from([1, 2, 3])
      await rejects(
          async () => {
            await stream.reduce(
              async (p, c, { signal }) => {
                signal.addEventListener('abort', () => {}, {
                  once: true
                })
                if (c === 3) {
                  await new Promise(() => {}) // Explicitly do not pass signal here
                }

                return Promise.resolve()
              },
              0,
              {
                signal: AbortSignal.abort()
              }
            )
          },
          {
            name: 'AbortError'
          }
        );
      strictEqual(stream.destroyed, true);
    }
    {
      // Error cases
      await rejects(() => Readable.from([]).reduce(1), /TypeError/);
      await rejects(() => Readable.from([]).reduce('5'), /TypeError/);
      await rejects(() => Readable.from([]).reduce((x, y) => x + y, 0, 1), /ERR_INVALID_ARG_TYPE/);
      await rejects(
        () =>
          Readable.from([]).reduce((x, y) => x + y, 0, {
            signal: true
          }),
        /ERR_INVALID_ARG_TYPE/
      );
    }
    {
      // Test result is a Promise
      const result = Readable.from([1, 2, 3, 4, 5]).reduce(sum, 0);
      ok(result instanceof Promise);
    }
  }
};

export const readablelistening_state = {
  async test(ctrl, env, ctx) {
    const r = new Readable({
      read: () => {}
    });

    // readableListening state should start in `false`.
    strictEqual(r._readableState.readableListening, false);
    const readableCalled = deferredPromise();
    const dataCalled = deferredPromise();
    r.on(
      'readable',
      () => {
        // Inside the readable event this state should be true.
        strictEqual(r._readableState.readableListening, true);
        readableCalled.resolve();
      }
    )
    r.push(Buffer.from('Testing readableListening state'))
    const r2 = new Readable({
      read: () => {}
    });

    // readableListening state should start in `false`.
    strictEqual(r2._readableState.readableListening, false);
    r2.on(
      'data',
      (chunk) => {
        // readableListening should be false because we don't have
        // a `readable` listener
        strictEqual(r2._readableState.readableListening, false);
        dataCalled.resolve();
      }
    )
    r2.push(Buffer.from('Testing readableListening state'))
    await Promise.all([
      readableCalled.promise,
      dataCalled.promise,
    ]);
  }
};

export const readable_with_unimplemented_read = {
  async test(ctrl, env, ctx) {
    const readable = new Readable();
    readable.read();
    const errored = deferredPromise();
    const closed = deferredPromise();
    readable.on(
      'error',
      function(err) {
        strictEqual(err.code, 'ERR_METHOD_NOT_IMPLEMENTED');
        errored.resolve();
      }
    );
    readable.on('close', closed.resolve);
    await Promise.all([
      errored.promise,
      closed.promise,
    ]);
  }
};

export const readable_unshift = {
  async test(ctrl, env, ctx) {
    {
      // Check that strings are saved as Buffer
      const readable = new Readable({
        read() {}
      });
      const string = 'abc';
      const dataCalled = deferredPromise();
      readable.on(
        'data',
        (chunk) => {
          ok(Buffer.isBuffer(chunk));
          strictEqual(chunk.toString('utf8'), string);
          dataCalled.resolve();
        }
      );
      readable.unshift(string);
      await dataCalled.promise;
    }
    {
      // Check that data goes at the beginning
      const readable = new Readable({
        read() {}
      });
      const unshift = 'front';
      const push = 'back';
      const expected = [unshift, push];
      const dataCalled = deferredPromise();
      let dataCount = 0;
      readable.on(
        'data',
        (chunk) => {
          strictEqual(chunk.toString('utf8'), expected.shift());
          if (++dataCount === 2) dataCalled.resolve();
        }
      );
      readable.push(push);
      readable.unshift(unshift);
      await dataCalled.promise;
    }
    {
      // Check that buffer is saved with correct encoding
      const readable = new Readable({
        read() {}
      });
      const encoding = 'base64';
      const string = Buffer.from('abc').toString(encoding);
      const dataCalled = deferredPromise();
      readable.on(
        'data',
        (chunk) => {
          strictEqual(chunk.toString(encoding), string);
          dataCalled.resolve();
        }
      );
      readable.unshift(string, encoding);
      await dataCalled.promise;
    }
    {
      const streamEncoding = 'base64';
      const dataCalled = deferredPromise();
      function checkEncoding(readable) {
        // chunk encodings
        const encodings = ['utf8', 'binary', 'hex', 'base64'];
        const expected = [];
        let dataCount = 0;
        readable.on(
          'data',
          (chunk) => {
            const { encoding, string } = expected.pop();
            strictEqual(chunk.toString(encoding), string);
            if (++dataCount === encodings.length) dataCalled.resolve();
          }
        );
        for (const encoding of encodings) {
          const string = 'abc';

          // If encoding is the same as the state.encoding the string is
          // saved as is
          const expect = encoding !== streamEncoding ? Buffer.from(string, encoding).toString(streamEncoding) : string;
          expected.push({
            encoding,
            string: expect
          });
          readable.unshift(string, encoding);
        }
      }
      const r1 = new Readable({
        read() {}
      });
      r1.setEncoding(streamEncoding);
      checkEncoding(r1);
      const r2 = new Readable({
        read() {},
        encoding: streamEncoding
      });
      checkEncoding(r2);
      await dataCalled.promise;
    }
    {
      // Both .push & .unshift should have the same behaviour
      // When setting an encoding, each chunk should be emitted with that encoding
      const encoding = 'base64';
      const dataCalled = deferredPromise();
      function checkEncoding(readable) {
        const string = 'abc';
        let dataCount = 0;
        readable.on(
          'data',
          (chunk) => {
            strictEqual(chunk, Buffer.from(string).toString(encoding));
            if (++dataCount === 2) dataCalled.resolve();
          }
        );
        readable.push(string);
        readable.unshift(string);
      }
      const r1 = new Readable({
        read() {}
      });
      r1.setEncoding(encoding);
      checkEncoding(r1);
      const r2 = new Readable({
        read() {},
        encoding
      });
      checkEncoding(r2);
      await dataCalled.promise;
    }
    {
      // Check that ObjectMode works
      const readable = new Readable({
        objectMode: true,
        read() {}
      });
      const chunks = ['a', 1, {}, []];
      const dataCalled = deferredPromise();
      let dataCount = 0;
      readable.on(
        'data',
        (chunk) => {
          strictEqual(chunk, chunks.pop());
          if (++dataCount === chunks.length) dataCalled.resolve();
        }
      );
      for (const chunk of chunks) {
        readable.unshift(chunk);
      }
      await dataCalled.promise;
    }
    {
      // Should not throw: https://github.com/nodejs/node/issues/27192
      const highWaterMark = 50;
      class ArrayReader extends Readable {
        constructor(opt) {
          super({
            highWaterMark
          });
          // The error happened only when pushing above hwm
          this.buffer = new Array(highWaterMark * 2).fill(0).map(String);
        }
        _read(size) {
          while (this.buffer.length) {
            const chunk = this.buffer.shift();
            if (!this.buffer.length) {
              this.push(chunk);
              this.push(null);
              return true;
            }
            if (!this.push(chunk)) return;
          }
        }
      }
      const readCalled = deferredPromise();
      function onRead() {
        while (null !== stream.read()) {
          // Remove the 'readable' listener before unshifting
          stream.removeListener('readable', onRead);
          stream.unshift('a');
          break;
        }
        readCalled.resolve();
      }
      const stream = new ArrayReader();
      stream.once('readable', onRead);
      await readCalled.promise;
    }
  }
};

export const readable_setencoding_null = {
  async test(ctrl, env, ctx) {
    const readable = new Readable({
      encoding: 'hex'
    });
    strictEqual(readable._readableState.encoding, 'hex');
    readable.setEncoding(null);
    strictEqual(readable._readableState.encoding, 'utf8');
  }
};

export const readable_setencoding_existing_buffers = {
  async test(ctrl, env, ctx) {
    {
      // Call .setEncoding() while there are bytes already in the buffer.
      const r = new Readable({
        read() {}
      });
      r.push(Buffer.from('a'));
      r.push(Buffer.from('b'));
      r.setEncoding('utf8');
      const chunks = [];
      r.on('data', (chunk) => chunks.push(chunk));
      queueMicrotask(() => {
        deepStrictEqual(chunks, ['ab']);
      });
    }
    {
      // Call .setEncoding() while the buffer contains a complete,
      // but chunked character.
      const r = new Readable({
        read() {}
      });
      r.push(Buffer.from([0xf0]));
      r.push(Buffer.from([0x9f]));
      r.push(Buffer.from([0x8e]));
      r.push(Buffer.from([0x89]));
      r.setEncoding('utf8');
      const chunks = [];
      r.on('data', (chunk) => chunks.push(chunk));
      queueMicrotask(() => {
        deepStrictEqual(chunks, ['🎉']);
      });
    }
    {
      // Call .setEncoding() while the buffer contains an incomplete character,
      // and finish the character later.
      const r = new Readable({
        read() {}
      });
      r.push(Buffer.from([0xf0]));
      r.push(Buffer.from([0x9f]));
      r.setEncoding('utf8');
      r.push(Buffer.from([0x8e]));
      r.push(Buffer.from([0x89]));
      const chunks = [];
      r.on('data', (chunk) => chunks.push(chunk));
      queueMicrotask(() => {
        deepStrictEqual(chunks, ['🎉']);
      });
    }
  }
};

export const readable_resumescheduled = {
  async test(ctrl, env, ctx) {
    {
      // pipe() test case
      const r = new Readable({
        read() {}
      });
      const w = new Writable();

      // resumeScheduled should start = `false`.
      strictEqual(r._readableState.resumeScheduled, false);

      // Calling pipe() should change the state value = true.
      r.pipe(w);
      strictEqual(r._readableState.resumeScheduled, true);
      queueMicrotask(
        () => {
          strictEqual(r._readableState.resumeScheduled, false);
        }
      );
    }
    {
      // 'data' listener test case
      const r = new Readable({
        read() {}
      });

      // resumeScheduled should start = `false`.
      strictEqual(r._readableState.resumeScheduled, false);
      r.push(Buffer.from([1, 2, 3]));

      // Adding 'data' listener should change the state value
      r.on(
        'data',
        () => {
          strictEqual(r._readableState.resumeScheduled, false);
        }
      );
      strictEqual(r._readableState.resumeScheduled, true);
      queueMicrotask(
        () => {
          strictEqual(r._readableState.resumeScheduled, false);
        }
      );
    }
    {
      // resume() test case
      const r = new Readable({
        read() {}
      });

      // resumeScheduled should start = `false`.
      strictEqual(r._readableState.resumeScheduled, false);

      // Calling resume() should change the state value.
      r.resume();
      strictEqual(r._readableState.resumeScheduled, true);
      r.on(
        'resume',
        () => {
          // The state value should be `false` again
          strictEqual(r._readableState.resumeScheduled, false);
        }
      );
      queueMicrotask(
        () => {
          strictEqual(r._readableState.resumeScheduled, false);
        }
      );
    }
  }
};

export const readable_resume_hwm = {
  async test(ctrl, env, ctx) {
    const readable = new Readable({
      read: () => { throw new Error('should not be called'); },
      highWaterMark: 100
    });

    // Fill up the internal buffer so that we definitely exceed the HWM:
    for (let i = 0; i < 10; i++) readable.push('a'.repeat(200));

    // Call resume, and pause after one chunk.
    // The .pause() is just so that we don’t empty the buffer fully, which would
    // be a valid reason to call ._read().
    readable.resume();
    const dataCalled = deferredPromise();
    readable.once(
      'data',
      () => {
        readable.pause();
        dataCalled.resolve();
      }
    );
    await dataCalled.promise;
  }
};

export const readable_reading_readingmore = {
  async test(ctrl, env, ctx) {
    {
      const readable = new Readable({
        read(size) {}
      });
      const state = readable._readableState;

      // Starting off with false initially.
      strictEqual(state.reading, false);
      strictEqual(state.readingMore, false);
      let dataCount = 0;
      let readableCount = 0;
      const dataCalled = deferredPromise();
      const readableCalled = deferredPromise();
      const ended = deferredPromise();
      readable.on(
        'data',
        (data) => {
          // While in a flowing state with a 'readable' listener
          // we should not be reading more
          if (readable.readableFlowing) strictEqual(state.readingMore, true);

          // Reading as long as we've not ended
          strictEqual(state.reading, !state.ended);
          if (++dataCount === 2) dataCalled.resolve();
        }
      );
      function onStreamEnd() {
        // End of stream; state.reading is false
        // And so should be readingMore.
        strictEqual(state.readingMore, false);
        strictEqual(state.reading, false);
        ended.resolve();
      }
      const expectedReadingMore = [true, true, false];
      readable.on(
        'readable',
        () => {
          // There is only one readingMore scheduled from on('data'),
          // after which everything is governed by the .read() call
          strictEqual(state.readingMore, expectedReadingMore.shift());

          // If the stream has ended, we shouldn't be reading
          strictEqual(state.ended, !state.reading);

          // Consume all the data
          while (readable.read() !== null);
          if (expectedReadingMore.length === 0)
            // Reached end of stream
            queueMicrotask(onStreamEnd);
          if (++readableCount === 3) readableCalled.resolve();
        }
      );
      readable.on('end', onStreamEnd);
      readable.push('pushed');
      readable.read(6);

      // reading
      strictEqual(state.reading, true);
      strictEqual(state.readingMore, true);

      // add chunk to front
      readable.unshift('unshifted');

      // end
      readable.push(null);
      await Promise.all([
        dataCalled.promise,
        readableCalled.promise,
        ended.promise,
      ]);
    }
    {
      const readable = new Readable({
        read(size) {}
      });
      const state = readable._readableState;

      // Starting off with false initially.
      strictEqual(state.reading, false);
      strictEqual(state.readingMore, false);
      let dataCount = 0;
      const dataCalled = deferredPromise();
      const ended = deferredPromise();
      readable.on(
        'data',
        (data) => {
          // While in a flowing state without a 'readable' listener
          // we should be reading more
          if (readable.readableFlowing) strictEqual(state.readingMore, true);

          // Reading as long as we've not ended
          strictEqual(state.reading, !state.ended);
          if (++dataCount === 2) dataCalled.resolve();
        }
      )
      function onStreamEnd() {
        // End of stream; state.reading is false
        // And so should be readingMore.
        strictEqual(state.readingMore, false);
        strictEqual(state.reading, false);
        ended.resolve();
      }
      readable.on('end', onStreamEnd);
      readable.push('pushed');

      // Stop emitting 'data' events
      strictEqual(state.flowing, true);
      readable.pause();

      // paused
      strictEqual(state.reading, false);
      strictEqual(state.flowing, false);
      readable.resume();
      strictEqual(state.reading, false);
      strictEqual(state.flowing, true);

      // add chunk to front
      readable.unshift('unshifted');

      // end
      readable.push(null);
      await Promise.all([
        dataCalled.promise,
        ended.promise,
      ]);
    }
    {
      const readable = new Readable({
        read(size) {}
      });
      const state = readable._readableState;

      let dataCount = 0;
      const dataCalled = deferredPromise();
      const ended = deferredPromise();

      // Starting off with false initially.
      strictEqual(state.reading, false);
      strictEqual(state.readingMore, false);
      readable.on('readable', fail);
      readable.on(
        'data',
        (data) => {
          // Reading as long as we've not ended
          strictEqual(state.reading, !state.ended);
          if (++dataCount === 2) dataCalled.resolve();
        }
      );
      readable.removeListener('readable', fail);
      function onStreamEnd() {
        // End of stream; state.reading is false
        // And so should be readingMore.
        strictEqual(state.readingMore, false);
        strictEqual(state.reading, false);
        ended.resolve();
      };
      readable.on('end', onStreamEnd);
      readable.push('pushed');

      // We are still not flowing, we will be resuming in the next tick
      strictEqual(state.flowing, false);

      // Wait for nextTick, so the readableListener flag resets
      queueMicrotask(function () {
        readable.resume();

        // Stop emitting 'data' events
        strictEqual(state.flowing, true);
        readable.pause();

        // paused
        strictEqual(state.flowing, false);
        readable.resume();
        strictEqual(state.flowing, true);

        // add chunk to front
        readable.unshift('unshifted');

        // end
        readable.push(null);
      });

      await Promise.all([
        dataCalled.promise,
        ended.promise,
      ]);
    }
  }
};

export const readable_readable = {
  async test(ctrl, env, ctx) {
    {
      const r = new Readable({
        read() {}
      });
      strictEqual(r.readable, true);
      r.destroy();
      strictEqual(r.readable, false);
    }
    {
      const r = new Readable({
        read() {}
      });
      strictEqual(r.readable, true);
      r.on('end', fail);
      r.resume();
      r.push(null);
      strictEqual(r.readable, true);
      r.off('end', fail);
      const ended = deferredPromise();
      r.on(
        'end',
        () => {
          strictEqual(r.readable, false);
          ended.resolve();
        }
      );
    }
    {
      const readCalled = deferredPromise();
      const errored = deferredPromise();
      const r = new Readable({
        read: () => {
          queueMicrotask(() => {
            r.destroy(new Error());
            strictEqual(r.readable, false);
            readCalled.resolve();
          })
        }
      })
      r.resume();
      r.on(
        'error',
        () => {
          strictEqual(r.readable, false);
          errored.resolve();
        }
      );
      await Promise.all([
        readCalled.promise,
        errored.promise,
      ]);
    }
  }
};

export const readable_readable_then_resume = {
  async test(ctrl, env, ctx) {
    await check(
      new Readable({
        objectMode: true,
        highWaterMark: 1,
        read() {
          if (!this.first) {
            this.push('hello');
            this.first = true;
            return;
          }
          this.push(null);
        }
      })
    );
    async function check(s) {
      const ended = deferredPromise();
      s.on('readable', ended.reject);
      s.on('end', ended.resolve);
      strictEqual(s.removeListener, s.off);
      s.removeListener('readable', ended.reject);
      s.resume();
      await ended.promise;
    }
  }
};

export const readable_pause_and_resume = {
  async test(ctrl, env, ctx) {
    let ticks = 18;
    let expectedData = 19;
    const rs = new Readable({
      objectMode: true,
      read: () => {
        if (ticks-- > 0) return queueMicrotask(() => rs.push({}));
        rs.push({});
        rs.push(null);
      }
    })
    const ended = deferredPromise();
    const ondataCalled = deferredPromise();
    rs.on('end', ended.resolve);
    await readAndPause();
    async function readAndPause() {
      // Does a on(data) -> pause -> wait -> resume -> on(data) ... loop.
      // Expects on(data) to never fire if the stream is paused.
      const ondata = (data) => {
        rs.pause();
        expectedData--;
        if (expectedData <= 0) return;
        queueMicrotask(function () {
          rs.removeListener('data', ondata);
          readAndPause();
          rs.resume();
          ondataCalled.resolve();
        })
      }; // Only call ondata once

      rs.on('data', ondata);
      await Promise.all([
        ended.promise,
        ondataCalled.promise,
      ]);
    }
    {
      const readable = new Readable({
        read() {}
      });
      function read() {};
      readable.setEncoding('utf8');
      readable.on('readable', read);
      readable.removeListener('readable', read);
      readable.pause();
      queueMicrotask(function () {
        ok(readable.isPaused());
      });
    }
    {
      const source3 = new PassThrough();
      const target3 = new PassThrough();
      const chunk = Buffer.allocUnsafe(1000);
      while (target3.write(chunk));
      source3.pipe(target3);
      const drainCalled = deferredPromise();
      target3.on(
        'drain',
        () => {
          ok(!source3.isPaused());
          drainCalled.resolve();
        }
      );
      target3.on('data', () => {});
      await drainCalled.promise;
    }
  }
};

export const readable_object_multi_push_async = {
  async test(ctrl, env, ctx) {
    const MAX = 42
    const BATCH = 10
    {
      const readCalled = deferredPromise();
      const ended = deferredPromise();
      let readCount = 0;
      const readable = new Readable({
        objectMode: true,
        read: function () {
          fetchData((err, data) => {
            if (err) {
              this.destroy(err);
              return;
            }
            if (data.length === 0) {
              this.push(null);
              return;
            }
            data.forEach((d) => this.push(d));
          });
          if (++readCount === Math.floor(MAX / BATCH) + 2)
            readCalled.resolve();
        }
      })
      let i = 0;
      function fetchData(cb) {
        if (i > MAX) {
          setTimeout(cb, 10, null, []);
        } else {
          const array = [];
          const max = i + BATCH;
          for (; i < max; i++) {
            array.push(i);
          }
          setTimeout(cb, 10, null, array);
        }
      }
      readable.on('readable', () => {
        let data;
        while ((data = readable.read()) !== null) {}
      });
      readable.on(
        'end',
        () => {
          strictEqual(i, (Math.floor(MAX / BATCH) + 1) * BATCH);
          ended.resolve();
        }
      );
      await Promise.all([
        readCalled.promise,
        ended.promise,
      ]);
    }
    {
      const readCalled = deferredPromise();
      const ended = deferredPromise();
      let readCount = 0;
      const readable = new Readable({
        objectMode: true,
        read: function () {
          fetchData((err, data) => {
            if (err) {
              this.destroy(err);
              return;
            }
            if (data.length === 0) {
              this.push(null);
              return;
            }
            data.forEach((d) => this.push(d));
          });
          if (++readCount === Math.floor(MAX / BATCH) + 2)
            readCalled.resolve();
        }
      });
      let i = 0;
      function fetchData(cb) {
        if (i > MAX) {
          setTimeout(cb, 10, null, []);
        } else {
          const array = [];
          const max = i + BATCH;
          for (; i < max; i++) {
            array.push(i);
          }
          setTimeout(cb, 10, null, array);
        }
      }
      readable.on('data', (data) => {});
      readable.on(
        'end',
        () => {
          strictEqual(i, (Math.floor(MAX / BATCH) + 1) * BATCH);
          ended.resolve();
        }
      );
      await Promise.all([
        readCalled.promise,
        ended.promise,
      ]);
    }
    {
      const readCalled = deferredPromise();
      const ended = deferredPromise();
      let readCount = 0;
      const readable = new Readable({
        objectMode: true,
        read: function () {
          fetchData((err, data) => {
            if (err) {
              this.destroy(err);
              return;
            }
            data.forEach((d) => this.push(d));
            if (data[BATCH - 1] >= MAX) {
              this.push(null);
            }
          });
          if (++readCount === Math.floor(MAX / BATCH) + 1)
            readCalled.resolve();
        }
      });
      let i = 0;
      function fetchData(cb) {
        const array = [];
        const max = i + BATCH;
        for (; i < max; i++) {
          array.push(i);
        }
        setTimeout(cb, 10, null, array);
      }
      readable.on('data', (data) => {});
      readable.on(
        'end',
        () => {
          strictEqual(i, (Math.floor(MAX / BATCH) + 1) * BATCH);
          ended.resolve();
        }
      );
      await Promise.all([
        readCalled.promise,
        ended.promise,
      ]);
    }
    {
      const ended = deferredPromise();
      const readable = new Readable({
        objectMode: true,
        read() {},
      });
      readable.on('data', ended.reject);
      readable.push(null);
      let nextTickPassed = false;
      queueMicrotask(() => {
        nextTickPassed = true;
      });
      readable.on(
        'end',
        () => {
          strictEqual(nextTickPassed, true);
          ended.resolve();
        }
      );
      await ended.promise;
    }
    {
      const ended = deferredPromise();
      const readCalled = deferredPromise();
      const readable = new Readable({
        objectMode: true,
        read: readCalled.resolve,
      });
      readable.on('data', (data) => {});
      readable.on('end', ended.resolve);
      queueMicrotask(() => {
        readable.push('aaa');
        readable.push(null);
      })
      await Promise.all([
        readCalled.promise,
        ended.promise,
      ]);
    }
  }
};

export const readable_no_unneeded_readable = {
  async test(ctrl, env, ctx) {
    async function test(r) {
      const wrapper = new Readable({
        read: () => {
          let data = r.read();
          if (data) {
            wrapper.push(data);
            return;
          }
          r.once('readable', function () {
            data = r.read();
            if (data) {
              wrapper.push(data);
            }
            // else: the end event should fire
          })
        }
      })

      r.once('end', function () {
        wrapper.push(null);
      });
      const ended = deferredPromise();
      wrapper.resume();
      wrapper.once('end', ended.resolve);
      await ended.promise;
    }
    {
      const source = new Readable({
        read: () => {}
      });
      source.push('foo');
      source.push('bar');
      source.push(null);
      const pt = source.pipe(new PassThrough());
      await test(pt);
    }
    {
      // This is the underlying cause of the above test case.
      const pushChunks = ['foo', 'bar'];
      const r = new Readable({
        read: () => {
          const chunk = pushChunks.shift();
          if (chunk) {
            // synchronous call
            r.push(chunk);
          } else {
            // asynchronous call
            queueMicrotask(() => r.push(null));
          }
        }
      })
      await test(r);
    }
  }
};

export const readable_next_no_null = {
  async test(ctrl, env, ctx) {
    async function* generate() {
      yield null;
    }
    const stream = Readable.from(generate());
    const errored = deferredPromise();
    stream.on(
      'error',
      function(err) {
        strictEqual(err.code, 'ERR_STREAM_NULL_VALUES');
        errored.resolve();
      }
    )
    stream.on('data', errored.reject);
    stream.on('end', errored.reject);
    await errored.promise;
  }
};

export const readable_needreadable = {
  async test(ctrl, env, ctx) {
    const readable = new Readable({
      read: () => {}
    });

    // Initialized to false.
    strictEqual(readable._readableState.needReadable, false);
    const readableCalled1 = deferredPromise();
    const ended = deferredPromise();
    readable.on(
      'readable',
      () => {
        // When the readable event fires, needReadable is reset.
        strictEqual(readable._readableState.needReadable, false, '1');
        readable.read();
        readableCalled1.resolve();
      }
    );

    // If a readable listener is attached, then a readable event is needed.
    strictEqual(readable._readableState.needReadable, true);
    readable.push('foo');
    readable.push(null);
    readable.on(
      'end',
      () => {
        // No need to emit readable anymore when the stream ends.
        strictEqual(readable._readableState.needReadable, false, '2');
        ended.resolve();
      }
    )
    await Promise.all([
      readableCalled1.promise,
      ended.promise,
    ]);

    const readableCalled2 = deferredPromise();
    const ended2 = deferredPromise();
    const dataCalled = deferredPromise();
    let readableCount = 0;
    let dataCount = 0;
    const asyncReadable = new Readable({
      read: () => {}
    });

    asyncReadable.on(
      'readable',
      () => {
        if (asyncReadable.read() !== null) {
          // After each read(), the buffer is empty.
          // If the stream doesn't end now,
          // then we need to notify the reader on future changes.
          readableCalled2.resolve();
        }
      }
    )
    queueMicrotask(
      () => {
        asyncReadable.push('foooo');
      }
    );
    queueMicrotask(
      () => {
        asyncReadable.push('bar');
      }
    );
    queueMicrotask(
      () => {
        asyncReadable.push(null);
        strictEqual(asyncReadable._readableState.needReadable, false);
      }
    )
    const flowing = new Readable({
      read: () => {}
    });

    // Notice this must be above the on('data') call.
    flowing.push('foooo');
    flowing.push('bar');
    flowing.push('quo');
    queueMicrotask(
      () => {
        flowing.push(null);
      }
    );

    // When the buffer already has enough data, and the stream is
    // in flowing mode, there is no need for the readable event.
    flowing.on(
      'data',
      function (data) {
        strictEqual(flowing._readableState.needReadable, false);
        if (++dataCount === 3) dataCalled.resolve();
      }
    );
    await Promise.all([
      readableCalled2.promise,
      dataCalled.promise,
      ended.promise,
    ]);

    // const slowProducer = new Readable({
    //   read: () => {}
    // });
    // const readableCalled3 = deferredPromise();
    // readableCount = 0;
    // slowProducer.on(
    //   'readable',
    //   () => {
    //     const chunk = slowProducer.read(8);
    //     const state = slowProducer._readableState;
    //     if (chunk === null) {
    //       // The buffer doesn't have enough data, and the stream is not need,
    //       // we need to notify the reader when data arrives.
    //       strictEqual(state.needReadable, true);
    //     } else {
    //       strictEqual(state.needReadable, false);
    //     }
    //     if (++readableCount === 4) readableCalled3.resolve();
    //   }
    // );
    // queueMicrotask(
    //   () => {
    //     slowProducer.push('foo')
    //     queueMicrotask(
    //       () => {
    //         slowProducer.push('foo')
    //         queueMicrotask(
    //           () => {
    //             slowProducer.push('foo')
    //             queueMicrotask(
    //               () => {
    //                 slowProducer.push(null)
    //               }
    //             );
    //           }
    //         );
    //       }
    //     );
    //   }
    // );
  }
};

export const readable_invalid_chunk = {
  async test(ctrl, env, ctx) {
    async function testPushArg(val) {
      const readable = new Readable({
        read: () => {}
      });
      const errored = deferredPromise();
      readable.on(
        'error',
        function(err) {
          strictEqual(err.code, 'ERR_INVALID_ARG_TYPE');
          errored.resolve();
        }
      );
      readable.push(val);
      await errored.promise;
    }
    await Promise.all([
      testPushArg([]),
      testPushArg({}),
      testPushArg(0)
    ]);
    async function testUnshiftArg(val) {
      const readable = new Readable({
        read: () => {}
      });
      const errored = deferredPromise();
      readable.on(
        'error',
        function(err) {
          strictEqual(err.code, 'ERR_INVALID_ARG_TYPE');
          errored.resolve();
        }
      );
      readable.unshift(val);
      await errored.promise;
    }
    await Promise.all([
      testUnshiftArg([]),
      testUnshiftArg({}),
      testUnshiftArg(0)
    ]);
  }
};

export const readable_hwm_0 = {
  async test(ctrl, env, ctx) {
    const readableCalled = deferredPromise();
    const readCalled = deferredPromise();
    const ended = deferredPromise();
    const r = new Readable({
      // Must be called only once upon setting 'readable' listener
      read: readCalled.resolve,
      highWaterMark: 0
    });
    let pushedNull = false;
    // This will trigger read(0) but must only be called after push(null)
    // because the we haven't pushed any data
    r.on(
      'readable',
      () => {
        strictEqual(r.read(), null);
        strictEqual(pushedNull, true);
        readableCalled.resolve();
      }
    )
    r.on('end', ended.resolve);
    queueMicrotask(() => {
      strictEqual(r.read(), null);
      pushedNull = true;
      r.push(null);
    });
    await Promise.all([
      readableCalled.promise,
      readCalled.promise,
      ended.promise,
    ]);
  }
};

export const readable_hwm_0_async = {
  async test(ctrl, env, ctx) {
    let count = 5;
    const readCalled = deferredPromise();
    const ended = deferredPromise();
    const dataCalled = deferredPromise();
    let readCount = 0;
    let dataCount = 0;
    const r = new Readable({
      // Called 6 times: First 5 return data, last one signals end of stream.
      read: () => {
        queueMicrotask(
          () => {
            if (count--) r.push('a');
            else r.push(null);
          }
        );
        if (++readCount === 6) readCalled.resolve();
      },
      highWaterMark: 0
    })
    r.on('end', ended.resolve);
    r.on('data', () => {
      if (++dataCount === 5) dataCalled.resolve();
    });
    await Promise.all([
      readCalled.promise,
      ended.promise,
      dataCalled.promise,
    ]);
  }
};

export const readable_event = {
  async test(ctrl, env, ctx) {
    {
      // First test, not reading when the readable is added.
      // make sure that on('readable', ...) triggers a readable event.
      const r = new Readable({
        highWaterMark: 3
      });
      const readableCalled = deferredPromise();
      r._read = readableCalled.reject;

      // This triggers a 'readable' event, which is lost.
      r.push(Buffer.from('blerg'));
      setTimeout(function () {
        // We're testing what we think we are
        ok(!r._readableState.reading);
        r.on('readable', readableCalled.resolve);
      }, 1);
      await readableCalled.promise;
    }
    {
      // Second test, make sure that readable is re-emitted if there's
      // already a length, while it IS reading.

      const r = new Readable({
        highWaterMark: 3
      });
      const readCalled = deferredPromise();
      const readableCalled = deferredPromise();
      r._read = readCalled.resolve;

      // This triggers a 'readable' event, which is lost.
      r.push(Buffer.from('bl'));
      setTimeout(function () {
        // Assert we're testing what we think we are
        ok(r._readableState.reading);
        r.on('readable', readableCalled.resolve);
      }, 1);
      await Promise.all([
        readCalled.promise,
        readableCalled.promise,
      ]);
    }
    {
      // Third test, not reading when the stream has not passed
      // the highWaterMark but *has* reached EOF.
      const r = new Readable({
        highWaterMark: 30
      });

      // This triggers a 'readable' event, which is lost.
      r.push(Buffer.from('blerg'));
      r.push(null);
      const readableCalled = deferredPromise();
      r._read = readableCalled.reject;
      setTimeout(function () {
        // Assert we're testing what we think we are
        ok(!r._readableState.reading);
        r.on('readable', readableCalled.resolve);
      }, 1);
      await readableCalled.promise;
    }
    {
      // Pushing an empty string in non-objectMode should
      // trigger next `read()`.
      const underlyingData = ['', 'x', 'y', '', 'z'];
      const expected = underlyingData.filter((data) => data);
      const result = [];
      const r = new Readable({
        encoding: 'utf8'
      });
      r._read = function () {
        queueMicrotask(() => {
          if (!underlyingData.length) {
            this.push(null);
          } else {
            this.push(underlyingData.shift());
          }
        });
      }
      r.on('readable', () => {
        const data = r.read();
        if (data !== null) result.push(data);
      });
      const ended = deferredPromise();
      r.on(
        'end',
        () => {
          deepStrictEqual(result, expected);
          ended.resolve();
        }
      );
      await ended.promise;
    }
    {
      // #20923
      const r = new Readable();
      r._read = function () {
        // Actually doing thing here
      }
      r.on('data', function () {});
      r.removeAllListeners();
      strictEqual(r.eventNames().length, 0);
    }
  }
};

export const readable_error_end = {
  async test(ctrl, env, ctx) {
    const r = new Readable({
      read() {}
    });
    const data = deferredPromise();
    const errored = deferredPromise();
    r.on('end', errored.reject);
    r.on('data', data.resolve);
    r.on('error', errored.resolve);
    r.push('asd');
    r.push(null);
    r.destroy(new Error('kaboom'));
    await Promise.all([
      data.promise,
      errored.promise,
    ]);
  }
};

export const readable_ended = {
  async test(ctrl, env, ctx) {
    {
      // Find it on Readable.prototype
      ok(Reflect.has(Readable.prototype, 'readableEnded'));
    }

    // event
    {
      const readable = new Readable();
      readable._read = () => {
        // The state ended should start in false.
        strictEqual(readable.readableEnded, false);
        readable.push('asd');
        strictEqual(readable.readableEnded, false);
        readable.push(null);
        strictEqual(readable.readableEnded, false);
      }
      const ended = deferredPromise();
      const dataCalled = deferredPromise();
      readable.on(
        'end',
        () => {
          strictEqual(readable.readableEnded, true);
          ended.resolve();
        }
      );
      readable.on(
        'data',
        () => {
          strictEqual(readable.readableEnded, false);
          dataCalled.resolve();
        }
      );
      await Promise.all([
        ended.promise,
        dataCalled.promise,
      ]);
    }

    // Verifies no `error` triggered on multiple .push(null) invocations
    {
      const readable = new Readable();
      readable.on('readable', () => {
        readable.read()
      });
      const ended = deferredPromise();
      readable.on('error', ended.reject);
      readable.on('end', ended.resolve);
      readable.push('a');
      readable.push(null);
      readable.push(null);
      await ended.promise;
    }
  }
};

export const readable_end_destroyed = {
  async test(ctrl, env, ctx) {
    const r = new Readable()
    const closed = deferredPromise();
    r.on('end', fail);
    r.resume();
    r.destroy();
    r.on(
      'close',
      () => {
        r.push(null);
        closed.resolve();
      }
    );
    await closed.promise;
  }
};

export const readable_short_stream = {
  async test(ctrl, env, ctx) {
    {
      const readCalled = deferredPromise();
      const transformCalled = deferredPromise();
      const flushCalled = deferredPromise();
      const readableCalled = deferredPromise();
      let readableCount = 0;
      const r = new Readable({
        read: function () {
          this.push('content');
          this.push(null);
          readCalled.resolve();
        }
      })
      const t = new Transform({
        transform: function (chunk, encoding, callback) {
          transformCalled.resolve();
          this.push(chunk);
          return callback();
        },
        flush: function (callback) {
          flushCalled.resolve();
          return callback();
        }
      })
      r.pipe(t);
      t.on(
        'readable',
        function () {
          while (true) {
            const chunk = t.read();
            if (!chunk) break;
            strictEqual(chunk.toString(), 'content');
          }
          if (++readableCount === 2) readableCalled.resolve();
        }
      );
      await Promise.all([
        readCalled.promise,
        transformCalled.promise,
        flushCalled.promise,
        readableCalled.promise,
      ]);
    }
    {
      const transformCalled = deferredPromise();
      const flushCalled = deferredPromise();
      const readableCalled = deferredPromise();
      const t = new Transform({
        transform: function (chunk, encoding, callback) {
          transformCalled.resolve();
          this.push(chunk);
          return callback();
        },
        flush: function (callback) {
          flushCalled.resolve();
          return callback();
        }
      });
      t.end('content');
      t.on(
        'readable',
        function () {
          while (true) {
            const chunk = t.read();
            if (!chunk) break;
            strictEqual(chunk.toString(), 'content');
          }
          readableCalled.resolve();
        }
      );
      await Promise.all([
        transformCalled.promise,
        flushCalled.promise,
        readableCalled.promise,
      ]);
    }
    {
      const transformCalled = deferredPromise();
      const flushCalled = deferredPromise();
      const readableCalled = deferredPromise();
      const t = new Transform({
        transform: function (chunk, encoding, callback) {
          transformCalled.resolve();
          this.push(chunk);
          return callback();
        },
        flush: function (callback) {
          flushCalled.resolve();
          return callback();
        }
      })
      t.write('content');
      t.end();
      t.on(
        'readable',
        function () {
          while (true) {
            const chunk = t.read();
            if (!chunk) break;
            strictEqual(chunk.toString(), 'content');
            readableCalled.resolve();
          }
        }
      );
      await Promise.all([
        transformCalled.promise,
        flushCalled.promise,
        readableCalled.promise,
      ]);
    }
    {
      const t = new Readable({
        read() {}
      });
      const readableCalled = deferredPromise();
      t.on(
        'readable',
        function () {
          while (true) {
            const chunk = t.read();
            if (!chunk) break;
            strictEqual(chunk.toString(), 'content');
          }
          readableCalled.resolve();
        }
      )
      t.push('content');
      t.push(null);
      await readableCalled.promise;
    }
    {
      const t = new Readable({
        read() {}
      });
      const readableCalled = deferredPromise();
      let readableCount = 0;
      t.on(
        'readable',
        function () {
          while (true) {
            const chunk = t.read()
            if (!chunk) break
            strictEqual(chunk.toString(), 'content')
          }
          if (++readableCount === 2) readableCalled.resolve();
        }
      )
      queueMicrotask(() => {
        t.push('content');
        t.push(null);
      });
      await readableCalled.promise;
    }
    {
      const transformCalled = deferredPromise();
      const flushCalled = deferredPromise();
      const readableCalled = deferredPromise();
      let readableCount = 0;
      const t = new Transform({
        transform: function (chunk, encoding, callback) {
          transformCalled.resolve();
          this.push(chunk);
          return callback();
        },
        flush: function (callback) {
          flushCalled.resolve();
          return callback();
        }
      })
      t.on(
        'readable',
        function () {
          while (true) {
            const chunk = t.read();
            if (!chunk) break;
            strictEqual(chunk.toString(), 'content');
          }
          if (++readableCount === 2) readableCalled.resolve();
        }
      );
      t.write('content');
      t.end();
      await Promise.all([
        transformCalled.promise,
        flushCalled.promise,
        readableCalled.promise,
      ]);
    }
  }
};

export const readable_didread = {
  async test(ctrl, env, ctx) {
    function noop() {}
    async function check(readable, data, fn) {
      const closed = deferredPromise();
      strictEqual(readable.readableDidRead, false);
      strictEqual(isDisturbed(readable), false);
      strictEqual(isErrored(readable), false);
      if (data === -1) {
        readable.on(
          'error',
          () => {
            strictEqual(isErrored(readable), true);
          }
        )
        readable.on('data', fail);
        readable.on('end', fail);
      } else {
        readable.on('error', fail)
        if (data === -2) {
          readable.on('end', fail)
        } else {
          readable.on('end', () => {});
        }
        if (data > 0) {
          readable.on('data', () => {});
        } else {
          readable.on('data', fail);
        }
      }
      readable.on('close', closed.resolve);
      fn();
      queueMicrotask(() => {
        strictEqual(readable.readableDidRead, data > 0);
        if (data > 0) {
          strictEqual(isDisturbed(readable), true);
        }
      });
      await closed.promise;
    }
    {
      const readable = new Readable({
        read() {
          this.push(null);
        }
      })
      await check(readable, 0, () => {
        readable.read();
      });
    }
    {
      const readable = new Readable({
        read() {
          this.push(null);
        }
      });
      await check(readable, 0, () => {
        readable.resume();
      });
    }
    {
      const readable = new Readable({
        read() {
          this.push(null);
        }
      })
      await check(readable, -2, () => {
        readable.destroy();
      });
    }
    {
      const readable = new Readable({
        read() {
          this.push(null);
        }
      });
      await check(readable, -1, () => {
        readable.destroy(new Error());
      });
    }
    {
      const readable = new Readable({
        read() {
          this.push('data');
          this.push(null);
        }
      });
      await check(readable, 1, () => {
        readable.on('data', noop);
      });
    }
    {
      const readable = new Readable({
        read() {
          this.push('data');
          this.push(null);
        }
      })
      await check(readable, 1, () => {
        readable.on('data', noop);
        readable.off('data', noop);
      });
    }
  }
};

export const readable_destroy = {
  async test(ctrl, env, ctx) {
    {
      const closed = deferredPromise();
      const read = new Readable({
        read() {}
      });
      read.resume();
      read.on('close', closed.resolve);
      read.destroy();
      strictEqual(read.errored, null);
      strictEqual(read.destroyed, true);
      await closed.promise;
    }
    {
      const read = new Readable({
        read() {}
      })
      const closed = deferredPromise();
      const errored = deferredPromise();
      read.resume();
      const expected = new Error('kaboom');
      read.on('end', closed.reject);
      read.on('close', closed.resolve);
      read.on(
        'error',
        (err) => {
          strictEqual(err, expected);
          errored.resolve();
        }
      )
      read.destroy(expected);
      strictEqual(read.errored, expected);
      strictEqual(read.destroyed, true);
      await Promise.all([
        closed.promise,
        errored.promise,
      ]);
    }
    {
      const destroyCalled = deferredPromise();
      const closed = deferredPromise();
      const errored = deferredPromise();
      const read = new Readable({
        read() {}
      });
      read._destroy = function (err, cb) {
        strictEqual(err, expected);
        cb(err);
        destroyCalled.resolve();
      };
      const expected = new Error('kaboom');
      read.on('end', closed.reject);
      read.on('close', closed.resolve);
      read.on(
        'error',
        (err) => {
          strictEqual(err, expected);
          errored.resolve();
        }
      )
      read.destroy(expected);
      strictEqual(read.destroyed, true);
      await Promise.all([
        destroyCalled.promise,
        closed.promise,
        errored.promise,
      ]);
    }
    {
      const destroyCalled = deferredPromise();
      const closed = deferredPromise();
      const read = new Readable({
        read() {},
        destroy: function (err, cb) {
          strictEqual(err, expected);
          cb();
          destroyCalled.resolve();
        }
      });
      const expected = new Error('kaboom');
      read.on('end', closed.reject);

      // Error is swallowed by the custom _destroy
      read.on('error', fail);
      read.on('close', closed.resolve);
      read.destroy(expected);
      strictEqual(read.destroyed, true);
      await Promise.all([
        destroyCalled.promise,
        closed.promise,
      ]);
    }
    {
      const read = new Readable({
        read() {}
      });
      const destroyCalled = deferredPromise();
      read._destroy = function (err, cb) {
        strictEqual(err, null);
        cb();
        destroyCalled.resolve();
      };
      read.destroy();
      strictEqual(read.destroyed, true);
      await destroyCalled.promise;
    }
    {
      const read = new Readable({
        read() {}
      });
      read.resume();
      const closed = deferredPromise();
      const destroyCalled = deferredPromise();
      read._destroy = function (err, cb) {
        strictEqual(err, null);
        queueMicrotask(() => {
          this.push(null);
          cb();
          destroyCalled.resolve();
        });
      };
      read.on('end', fail);
      read.on('close', closed.resolve);
      read.destroy();
      read.removeListener('end', fail);
      read.on('end', closed.reject);
      strictEqual(read.destroyed, true);
      await Promise.all([
        closed.promise,
        destroyCalled.promise,
      ]);
    }
    {
      const read = new Readable({
        read() {}
      });
      const expected = new Error('kaboom');
      const destroyCalled = deferredPromise();
      const closed = deferredPromise();
      const errored = deferredPromise();
      read._destroy = function (err, cb) {
        strictEqual(err, null);
        cb(expected);
        destroyCalled.resolve();
      };
      let ticked = false;
      read.on('end', closed.reject);
      read.on('close', closed.resolve);
      read.on(
        'error',
        (err) => {
          strictEqual(ticked, true);
          strictEqual(read._readableState.errorEmitted, true);
          strictEqual(read._readableState.errored, expected);
          strictEqual(err, expected);
          errored.resolve();
        }
      )
      read.destroy();
      strictEqual(read._readableState.errorEmitted, false);
      strictEqual(read._readableState.errored, expected);
      strictEqual(read.destroyed, true);
      ticked = true;
      await Promise.all([
        destroyCalled.promise,
        closed.promise,
        errored.promise,
      ]);
    }
    {
      function MyReadable() {
        strictEqual(this.destroyed, false);
        this.destroyed = false;
        Readable.call(this);
      }
      Object.setPrototypeOf(MyReadable.prototype, Readable.prototype);
      Object.setPrototypeOf(MyReadable, Readable);
      new MyReadable();
    }
    {
      // Destroy and destroy callback
      const read = new Readable({
        read() {}
      });
      read.resume();
      const expected = new Error('kaboom');
      let ticked = false;
      const closed = deferredPromise();
      const destroyCalled = deferredPromise();
      const errored = deferredPromise();
      read.on(
        'close',
        () => {
          strictEqual(read._readableState.errorEmitted, true);
          strictEqual(ticked, true);
          closed.resolve();
        }
      )
      read.on(
        'error',
        (err) => {
          strictEqual(err, expected);
          errored.resolve();
        }
      )
      strictEqual(read._readableState.errored, null);
      strictEqual(read._readableState.errorEmitted, false);
      read.destroy(
        expected,
        function (err) {
          strictEqual(read._readableState.errored, expected);
          strictEqual(err, expected);
          destroyCalled.resolve();
        }
      )
      strictEqual(read._readableState.errorEmitted, false);
      strictEqual(read._readableState.errored, expected);
      ticked = true;
      await Promise.all([
        closed.promise,
        destroyCalled.promise,
        errored.promise,
      ]);
    }
    {
      const destroyCalled = deferredPromise();
      const closed = deferredPromise();
      const errored = deferredPromise();
      const readable = new Readable({
        destroy: function (err, cb) {
          queueMicrotask(() => cb(new Error('kaboom 1')));
          destroyCalled.resolve();
        },
        read() {}
      });
      let ticked = false;
      readable.on(
        'close',
        () => {
          strictEqual(ticked, true);
          strictEqual(readable._readableState.errorEmitted, true);
          closed.resolve();
        }
      );
      readable.on(
        'error',
        (err) => {
          strictEqual(ticked, true);
          strictEqual(err.message, 'kaboom 1');
          strictEqual(readable._readableState.errorEmitted, true);
          errored.resolve();
        }
      )
      readable.destroy();
      strictEqual(readable.destroyed, true);
      strictEqual(readable._readableState.errored, null);
      strictEqual(readable._readableState.errorEmitted, false);

      // Test case where `readable.destroy()` is called again with an error before
      // the `_destroy()` callback is called.
      readable.destroy(new Error('kaboom 2'));
      strictEqual(readable._readableState.errorEmitted, false);
      strictEqual(readable._readableState.errored, null);
      ticked = true;

      await Promise.all([
        destroyCalled.promise,
        closed.promise,
        errored.promise,
      ]);
    }
    {
      const read = new Readable({
        read() {}
      });
      const closed = deferredPromise();
      read.destroy();
      read.push('hi');
      read.on('data', fail);
      read.on('close', closed.resolve);
      await closed.promise;
    }
    {
      const closed = deferredPromise();
      const read = new Readable({
        read: closed.reject
      })
      read.on('close', closed.resolve);
      read.destroy();
      strictEqual(read.destroyed, true);
      read.read();
      await closed.promise;
    }
    {
      const read = new Readable({
        autoDestroy: false,
        read() {
          this.push(null);
          this.push('asd');
        }
      });
      const errored = deferredPromise();
      read.on(
        'error',
        () => {
          ok(read._readableState.errored);
          errored.resolve();
        }
      )
      read.resume();
      await errored.promise;
    }
    {
      const controller = new AbortController();
      const read = addAbortSignal(
        controller.signal,
        new Readable({
          read() {
            this.push('asd');
          }
        })
      );
      const closed = deferredPromise();
      const errored = deferredPromise();
      read.on(
        'error',
        (e) => {
          strictEqual(e.name, 'AbortError');
          errored.resolve();
        }
      )
      controller.abort();
      read.on('data', closed.reject);
      read.on('close', closed.resolve);
      await Promise.all([
        closed.promise,
        errored.promise,
      ]);
    }
    {
      const controller = new AbortController()
      const read = new Readable({
        signal: controller.signal,
        read() {
          this.push('asd')
        }
      })
      const closed = deferredPromise();
      const errored = deferredPromise();
      read.on(
        'error',
        (e) => {
          strictEqual(e.name, 'AbortError');
          errored.resolve();
        }
      )
      controller.abort();
      read.on('data', closed.reject);
      read.on('close', closed.resolve);
      await Promise.all([
        closed.promise,
        errored.promise,
      ]);
    }
    {
      const controller = new AbortController()
      const read = addAbortSignal(
        controller.signal,
        new Readable({
          objectMode: true,
          read() {
            return false
          }
        })
      )
      read.push('asd')
      const errored = deferredPromise();
      read.on(
        'error',
        (e) => {
          strictEqual(e.name, 'AbortError');
          errored.resolve();
        }
      )
      const rejected = rejects(
        (async () => {
          // eslint-disable-next-line no-unused-vars, no-empty
          for await (const chunk of read) {
          }
        })(),
        /AbortError/
      );
      setTimeout(() => controller.abort(), 0);
      await Promise.all([
        errored.promise,
        rejected,
      ]);
    }
    {
      const read = new Readable({
        read() {}
      });
      const closed = deferredPromise();
      const errored = deferredPromise();
      read.on('data', closed.reject);
      read.on(
        'error',
        (e) => {
          read.push('asd');
          read.read();
          errored.resolve();
        }
      )
      read.on(
        'close',
        (e) => {
          read.push('asd');
          read.read();
          closed.resolve();
        }
      )
      read.destroy(new Error('asd'));
      await Promise.all([
        closed.promise,
        errored.promise,
      ]);
    }
    {
      const read = new Readable({
        read() {}
      });
      const closed = deferredPromise();
      read.on('data', closed.reject);
      read.on(
        'close',
        (e) => {
          read.push('asd');
          read.read();
          closed.resolve();
        }
      )
      read.destroy();
      await closed.promise;
    }
    {
      const read = new Readable({
        read() {}
      });
      const closed = deferredPromise();
      read.on('data', closed.reject);
      read.on(
        'close',
        (e) => {
          read.push('asd');
          read.unshift('asd');
          closed.resolve();
        }
      )
      read.destroy();
      await closed.promise;
    }
    {
      const read = new Readable({
        read() {}
      })
      const closed = deferredPromise();
      read.on('data', closed.reject);
      read.on('close', closed.resolve);
      read.destroy();
      read.unshift('asd');
      await closed.promise;
    }
    {
      const read = new Readable({
        read() {}
      });
      read.resume();
      const closed = deferredPromise();
      read.on('data', closed.reject);
      read.on(
        'close',
        (e) => {
          read.push('asd');
          closed.resolve();
        }
      )
      read.destroy();
      await closed.promise;
    }
    {
      const read = new Readable({
        read() {}
      });
      const closed = deferredPromise();
      read.on('data', closed.reject);
      read.on('close', closed.resolve);
      read.destroy();
      read.push('asd');
      await closed.promise;
    }
  }
};

export const readable_data = {
  async test(ctrl, env, ctx) {
    const readable = new Readable({
      read() {}
    });
    function read() {}
    readable.setEncoding('utf8');
    readable.on('readable', read);
    readable.removeListener('readable', read);
    const dataCalled = deferredPromise();
    queueMicrotask(function () {
      readable.on('data', dataCalled.resolve);
      readable.push('hello');
    });
    await dataCalled.promise;
  }
};

export const readable_constructor_set_methods = {
  async test(ctrl, env, ctx) {
    const readCalled = deferredPromise();
    const _read = function _read(n) {
      this.push(null);
      readCalled.resolve();
    }
    const r = new Readable({
      read: _read
    });
    r.resume();
    await readCalled.promise;
  }
};

export const readable_add_chunk_during_data = {
  async test(ctrl, env, ctx) {
    const dataCalled = deferredPromise();
    let dataCount = 0;
    for (const method of ['push', 'unshift']) {
      const r = new Readable({
        read() {}
      });
      r.once(
        'data',
        (chunk) => {
          strictEqual(r.readableLength, 0);
          r[method](chunk);
          strictEqual(r.readableLength, chunk.length);
          r.on(
            'data',
            (chunk) => {
              strictEqual(chunk.toString(), 'Hello, world');
              if (++dataCount === 2) dataCalled.resolve();
            }
          )
        }
      );
      r.push('Hello, world');
    }
  }
};

export const readable_aborted = {
  async test(ctrl, env, ctx) {
    {
      const readable = new Readable({
        read() {}
      });
      strictEqual(readable.readableAborted, false);
      readable.destroy();
      strictEqual(readable.readableAborted, true);
    }
    {
      const readable = new Readable({
        read() {}
      });
      strictEqual(readable.readableAborted, false);
      readable.push(null);
      readable.destroy();
      strictEqual(readable.readableAborted, true);
    }
    {
      const readable = new Readable({
        read() {}
      });
      strictEqual(readable.readableAborted, false);
      readable.push('asd');
      readable.destroy();
      strictEqual(readable.readableAborted, true);
    }
    {
      const readable = new Readable({
        read() {}
      });
      strictEqual(readable.readableAborted, false);
      readable.push('asd');
      readable.push(null);
      strictEqual(readable.readableAborted, false);
      const ended = deferredPromise();
      readable.on(
        'end',
        () => {
          strictEqual(readable.readableAborted, false);
          readable.destroy();
          strictEqual(readable.readableAborted, false);
          queueMicrotask(() => {
            strictEqual(readable.readableAborted, false);
            ended.resolve();
          });
        }
      )
      readable.resume();
      await ended.promise;
    }
    {
      const duplex = new Duplex({
        readable: false,
        write() {}
      });
      duplex.destroy();
      strictEqual(duplex.readableAborted, false);
    }
  }
};

export const push_strings = {
  async test(ctrl, env, ctx) {
    class MyStream extends Readable {
      constructor(options) {
        super(options);
        this._chunks = 3;
      }
      _read(n) {
        switch (this._chunks--) {
          case 0:
            return this.push(null);
          case 1:
            return setTimeout(() => {
              this.push('last chunk');
            }, 100);
          case 2:
            return this.push('second to last chunk');
          case 3:
            return queueMicrotask(() => {
              this.push('first chunk');
            })
          default:
            throw new Error('?')
        }
      }
    }
    const ms = new MyStream();
    const results = [];
    ms.on('readable', function () {
      let chunk;
      while (null !== (chunk = ms.read())) results.push(String(chunk));
    })
    const expect = ['first chunksecond to last chunk', 'last chunk'];
    await promises.finished(ms);
    strictEqual(ms._chunks, -1);
    deepStrictEqual(results, expect);
  }
};

export const filter = {
  async test(ctrl, env, ctx) {
    {
      // Filter works on synchronous streams with a synchronous predicate
      const stream = Readable.from([1, 2, 3, 4, 5]).filter((x) => x < 3);
      const result = [1, 2];

      for await (const item of stream) {
        strictEqual(item, result.shift());
      }
    }
    {
      // Filter works on synchronous streams with an asynchronous predicate
      const stream = Readable.from([1, 2, 3, 4, 5]).filter(async (x) => {
        await Promise.resolve();
        return x > 3;
      })
      const result = [4, 5];
      for await (const item of stream) {
        strictEqual(item, result.shift());
      }
    }
    {
      // Map works on asynchronous streams with a asynchronous mapper
      const stream = Readable.from([1, 2, 3, 4, 5])
        .map(async (x) => {
          await Promise.resolve();
          return x + x;
        })
        .filter((x) => x > 5);
      const result = [6, 8, 10];
      for await (const item of stream) {
        strictEqual(item, result.shift());
      }
    }
    {
      // Filter works on an infinite stream
      const stream = Readable.from(
        (async function* () {
          while (true) yield 1;
        })()
      ).filter(async (x) => x < 3);
      let i = 1;
      for await (const item of stream) {
        strictEqual(item, 1);
        if (++i === 5) break;
      }
    }
    {
      // Filter works on constructor created streams
      let i = 0;
      const stream = new Readable({
        read() {
          if (i === 10) {
            this.push(null);
            return;
          }
          this.push(Uint8Array.from([i]));
          i++;
        },
        highWaterMark: 0
      }).filter(async ([x]) => x !== 5)
      const result = (await stream.toArray()).map((x) => x[0]);
      const expected = [...Array(10).keys()].filter((x) => x !== 5);
      deepStrictEqual(result, expected);
    }
    {
      // Throwing an error during `filter` (sync)
      const stream = Readable.from([1, 2, 3, 4, 5]).filter((x) => {
        if (x === 3) {
          throw new Error('boom');
        }
        return true;
      })
      await rejects(stream.map((x) => x + x).toArray(), /boom/);
    }
    {
      // Throwing an error during `filter` (async)
      const stream = Readable.from([1, 2, 3, 4, 5]).filter(async (x) => {
        if (x === 3) {
          throw new Error('boom');
        }
        return true;
      })
      await rejects(stream.filter(() => true).toArray(), /boom/);
    }
    {
      function once(ee, event) {
        const deferred = deferredPromise();
        ee.once(event, deferred.resolve);
        return deferred.promise;
      }
      // Concurrency + AbortSignal
      const ac = new AbortController();
      let calls = 0;
      const stream = Readable.from([1, 2, 3, 4]).filter(
        async (_, { signal }) => {
          calls++;
          await once(signal, 'abort');
        },
        {
          signal: ac.signal,
          concurrency: 2
        }
      );
      queueMicrotask(() => ac.abort());
      // pump
      await rejects(
          async () => {
            for await (const item of stream) {
              // nope
            }
          },
          {
            name: 'AbortError'
          }
        );
    }
    {
      // Concurrency result order
      const stream = Readable.from([1, 2]).filter(
        async (item, { signal }) => {
          await scheduler.wait(10 - item);
          return true
        },
        {
          concurrency: 2
        }
      );
      const expected = [1, 2];
      for await (const item of stream) {
        strictEqual(item, expected.shift());
      }
    }
    {
      // Error cases
      throws(() => Readable.from([1]).filter(1), /ERR_INVALID_ARG_TYPE/);
      throws(
        () =>
          Readable.from([1]).filter((x) => x, {
            concurrency: 'Foo'
          }), { code: 'ERR_OUT_OF_RANGE' }
      );
      throws(() => Readable.from([1]).filter((x) => x, 1), {
        code: 'ERR_INVALID_ARG_TYPE'
      });
    }
    {
      // Test result is a Readable
      const stream = Readable.from([1, 2, 3, 4, 5]).filter((x) => true);
      strictEqual(stream.readable, true);
    }
  }
};

export const pipeWithoutListenerCount = {
  async test(ctrl, env, ctx) {
    const r = new Stream();
    r.listenerCount = undefined;
    const w = new Stream();
    w.listenerCount = undefined;
    w.on('pipe', function () {
      r.emit('error', new Error('Readable Error'));
      w.emit('error', new Error('Writable Error'));
    });
    const rErrored = deferredPromise();
    const wErrored = deferredPromise();
    r.on('error', rErrored.resolve);
    w.on('error', wErrored.resolve);
    r.pipe(w);
    await Promise.all([
      rErrored.promise,
      wErrored.promise,
    ]);
  }
};

export const pipeUnpipeStreams = {
  async test(ctrl, env, ctx) {
    const source = Readable({
      read: () => {}
    });
    const dest1 = Writable({
      write: () => {}
    });
    const dest2 = Writable({
      write: () => {}
    });
    source.pipe(dest1);
    source.pipe(dest2);

    const unpipe1 = deferredPromise();
    const unpipe2 = deferredPromise();

    dest1.on('unpipe', unpipe1.resolve);
    dest2.on('unpipe', unpipe2.resolve);
    strictEqual(source._readableState.pipes[0], dest1);
    strictEqual(source._readableState.pipes[1], dest2);
    strictEqual(source._readableState.pipes.length, 2);

    // Should be able to unpipe them in the reverse order that they were piped.

    source.unpipe(dest2);
    deepStrictEqual(source._readableState.pipes, [dest1]);
    notStrictEqual(source._readableState.pipes, dest2);
    dest2.on('unpipe', fail);
    source.unpipe(dest2);
    source.unpipe(dest1);
    strictEqual(source._readableState.pipes.length, 0);
    {
      // Test `cleanup()` if we unpipe all streams.
      const source = Readable({
        read: () => {}
      });
      const dest1 = Writable({
        write: () => {}
      });
      const dest2 = Writable({
        write: () => {}
      });
      let destCount = 0;
      const srcCheckEventNames = ['end', 'data'];
      const destCheckEventNames = ['close', 'finish', 'drain', 'error', 'unpipe'];
      const checkSrcCleanup = () => {
        strictEqual(source._readableState.pipes.length, 0);
        strictEqual(source._readableState.flowing, false);
        srcCheckEventNames.forEach((eventName) => {
          strictEqual(source.listenerCount(eventName), 0, `source's '${eventName}' event listeners not removed`);
        })
      }
      async function checkDestCleanup(dest) {
        const done = deferredPromise();
        const currentDestId = ++destCount;
        source.pipe(dest);
        const unpipeChecker = () => {
          destCheckEventNames.forEach((eventName) => {
            strictEqual(
              dest.listenerCount(eventName),
              0,
              `destination{${currentDestId}}'s '${eventName}' event ` + 'listeners not removed'
            );
          });
          if (--destCount === 0) checkSrcCleanup();
          done.resolve();
        };
        dest.once('unpipe', unpipeChecker);
        await done.promise;
      }
      const p1 = checkDestCleanup(dest1);
      const p2 = checkDestCleanup(dest2);
      source.unpipe();
      await Promise.all([
        p1, p2, unpipe1.promise, unpipe2.promise
      ]);
    }
    {
      const src = Readable({
        read: () => {}
      });
      const dst = Writable({
        write: () => {}
      });
      src.pipe(dst);
      const resumeCalled = deferredPromise();
      src.on(
        'resume',
        () => {
          src.on('pause', resumeCalled.resolve);
          src.unpipe(dst);
        }
      );
      await resumeCalled.promise;
    }
  }
}

export const pipeSameDestinationTwice = {
  async test(ctrl, env, ctx) {
    // Regression test for https://github.com/nodejs/node/issues/12718.
    // Tests that piping a source stream twice to the same destination stream
    // works, and that a subsequent unpipe() call only removes the pipe *once*.
    {
      const passThrough = new PassThrough();
      const writeCalled = deferredPromise();
      const dest = new Writable({
        write: (chunk, encoding, cb) => {
          strictEqual(`${chunk}`, 'foobar');
          cb();
          writeCalled.resolve();
        }
      });
      passThrough.pipe(dest);
      passThrough.pipe(dest);
      strictEqual(passThrough._events.data.length, 2);
      strictEqual(passThrough._readableState.pipes.length, 2);
      strictEqual(passThrough._readableState.pipes[0], dest);
      strictEqual(passThrough._readableState.pipes[1], dest);
      passThrough.unpipe(dest);
      strictEqual(passThrough._events.data.length, 1);
      strictEqual(passThrough._readableState.pipes.length, 1);
      deepStrictEqual(passThrough._readableState.pipes, [dest]);
      passThrough.write('foobar');
      passThrough.pipe(dest);
      await writeCalled.promise;
    }
    {
      const passThrough = new PassThrough();
      const writeCalled = deferredPromise();
      let writeCount = 0;
      const dest = new Writable({
        write: (chunk, encoding, cb) => {
          strictEqual(`${chunk}`, 'foobar');
          cb();
          if (++writeCount == 2) writeCalled.resolve();
        }
      });
      passThrough.pipe(dest);
      passThrough.pipe(dest);
      strictEqual(passThrough._events.data.length, 2);
      strictEqual(passThrough._readableState.pipes.length, 2);
      strictEqual(passThrough._readableState.pipes[0], dest);
      strictEqual(passThrough._readableState.pipes[1], dest);
      passThrough.write('foobar');
      await writeCalled.promise;
    }
    {
      const passThrough = new PassThrough();
      const dest = new Writable({
        write: fail
      });
      passThrough.pipe(dest);
      passThrough.pipe(dest);
      strictEqual(passThrough._events.data.length, 2);
      strictEqual(passThrough._readableState.pipes.length, 2);
      strictEqual(passThrough._readableState.pipes[0], dest);
      strictEqual(passThrough._readableState.pipes[1], dest);
      passThrough.unpipe(dest);
      passThrough.unpipe(dest);
      strictEqual(passThrough._events.data, undefined);
      strictEqual(passThrough._readableState.pipes.length, 0);
      passThrough.write('foobar');
    }
  }
};

export const pipeNeedDrain = {
  async test(ctrl, env, ctx) {
    // Pipe should pause temporarily if writable needs drain.
    const w = new Writable({
      write(buf, encoding, callback) {
        queueMicrotask(callback);
      },
      highWaterMark: 1
    });
    while (w.write('asd'));
    strictEqual(w.writableNeedDrain, true);
    const r = new Readable({
      read() {
        this.push('asd');
        this.push(null);
      }
    });
    const pauseCalled = deferredPromise();
    const endCalled = deferredPromise();
    let pauseCount = 0;
    r.on('pause', () => {
      if (++pauseCount === 2) pauseCalled.resolve();
    });
    r.on('end', endCalled.resolve);
    r.pipe(w);
    await Promise.all([
      pauseCalled.promise,
      endCalled.promise,
    ]);
  }
};

export const pipeMultiplePipes = {
  async test(ctrl, env, ctx) {
    const readable = new Readable({
      read: () => {}
    });
    const writables = [];
    const promises = [];
    for (let i = 0; i < 5; i++) {
      const writeCalled = deferredPromise();
      promises.push(writeCalled.promise);
      const target = new Writable({
        write: (chunk, encoding, callback) => {
          target.output.push(chunk);
          callback();
          writeCalled.resolve();
        }
      });
      const pipeCalled = deferredPromise();
      promises.push(pipeCalled.promise);
      target.output = [];
      target.on('pipe', pipeCalled.resolve);
      readable.pipe(target);
      writables.push(target);
    }
    const input = Buffer.from([1, 2, 3, 4, 5]);
    readable.push(input);

    await Promise.all(promises);

    // The pipe() calls will postpone emission of the 'resume' event using nextTick,
    // so no data will be available to the writable streams until then.

    for (const target of writables) {
      deepStrictEqual(target.output, [input]);
      readable.unpipe(target);
    }
    readable.push('something else') // This does not get through.
    readable.push(null)
    readable.resume() // Make sure the 'end' event gets emitted.

    const ended = deferredPromise();
    readable.on(
      'end',
      () => {
        for (const target of writables) {
          deepStrictEqual(target.output, [input])
        }
        ended.resolve();
      }
    );
    await ended.promise;
  }
};

export const pipeManualResume = {
  async test(ctrl, env, ctx) {
    async function test(throwCodeInbetween) {
      // Check that a pipe does not stall if .read() is called unexpectedly
      // (i.e. the stream is not resumed by the pipe).

      const n = 1000;
      let counter = n;
      const rClosed = deferredPromise();
      const wClosed = deferredPromise();
      const rs = Readable({
        objectMode: true,
        read: () => {
          if (--counter >= 0)
            rs.push({ counter });
          else rs.push(null);
        }
      });
      const ws = Writable({
        objectMode: true,
        write: (data, enc, cb) => {
          queueMicrotask(cb);
        }
      });
      rs.on('close', rClosed.resolve);
      ws.on('close', wClosed.resolve);
      queueMicrotask(() => throwCodeInbetween(rs, ws));
      rs.pipe(ws);
      await Promise.all([
        rClosed.promise,
        wClosed.promise,
      ]);
    }

    await Promise.all([
      test((rs) => rs.read()),
      test((rs) => rs.resume()),
      test(() => 0),
    ]);
  }
};

export const pipeFlow = {
  async test(ctrl, env, ctx) {
    {
      let ticks = 17;
      const rs = new Readable({
        objectMode: true,
        read: () => {
          if (ticks-- > 0) return queueMicrotask(() => rs.push({}));
          rs.push({});
          rs.push(null);
        }
      });
      const ws = new Writable({
        highWaterMark: 0,
        objectMode: true,
        write: (data, end, cb) => queueMicrotask(cb)
      })
      const ended = deferredPromise();
      const finished = deferredPromise();
      rs.on('end', ended.resolve);
      ws.on('finish', finished.resolve);
      rs.pipe(ws);
      await Promise.all([
        ended.promise,
        finished.promise,
      ]);
    }
    {
      let missing = 8;
      const rs = new Readable({
        objectMode: true,
        read: () => {
          if (missing--) rs.push({});
          else rs.push(null);
        }
      })
      const pt = rs
        .pipe(
          new PassThrough({
            objectMode: true,
            highWaterMark: 2
          })
        )
        .pipe(
          new PassThrough({
            objectMode: true,
            highWaterMark: 2
          })
        );
      pt.on('end', () => {
        wrapper.push(null);
      });
      const wrapper = new Readable({
        objectMode: true,
        read: () => {
          queueMicrotask(() => {
            let data = pt.read();
            if (data === null) {
              pt.once('readable', () => {
                data = pt.read();
                if (data !== null) wrapper.push(data);
              });
            } else {
              wrapper.push(data);
            }
          });
        }
      });
      wrapper.resume();
      const ended = deferredPromise();
      wrapper.on('end', ended.resolve);
      await ended.promise;
    }
    {
      // Only register drain if there is backpressure.
      const rs = new Readable({
        read() {}
      });
      const pt = rs.pipe(
        new PassThrough({
          objectMode: true,
          highWaterMark: 2
        })
      );
      strictEqual(pt.listenerCount('drain'), 0);
      pt.on('finish', () => {
        strictEqual(pt.listenerCount('drain'), 0);
      });
      rs.push('asd');
      strictEqual(pt.listenerCount('drain'), 0);
      const done = deferredPromise();
      queueMicrotask(() => {
        rs.push('asd');
        strictEqual(pt.listenerCount('drain'), 0);
        rs.push(null);
        strictEqual(pt.listenerCount('drain'), 0);
        done.resolve();
      });
      await done.promise;
    }
  }
};

export const pipeErrorHandling = {
  async test(ctrl, env, ctx) {
    {
      const source = new Stream();
      const dest = new Stream();
      source.pipe(dest);
      let gotErr = null;
      source.on('error', function (err) {
        gotErr = err;
      });
      const err = new Error('This stream turned into bacon.');
      source.emit('error', err);
      strictEqual(gotErr, err);
    }
    {
      const source = new Stream();
      const dest = new Stream();
      source.pipe(dest);
      const err = new Error('This stream turned into bacon.');
      let gotErr = null;
      try {
        source.emit('error', err);
      } catch (e) {
        gotErr = e;
      }
      strictEqual(gotErr, err);
    }
    {
      const r = new Readable({
        autoDestroy: false
      });
      const w = new Writable({
        autoDestroy: false
      });
      let removed = false;
      r._read = function () {
        setTimeout(
          function () {
            ok(removed);
            throws(function () {
              w.emit('error', new Error('fail'));
            }, /^Error: fail$/)
          },
          1
        )
      };
      w.on('error', myOnError);
      r.pipe(w);
      w.removeListener('error', myOnError);
      removed = true;
      function myOnError() {
        throw new Error('this should not happen');
      }
    }
    {
      const r = new Readable();
      const w = new Writable();
      let removed = false;
      r._read = function () {
        setTimeout(
          function () {
            ok(removed);
            w.emit('error', new Error('fail'));
          },
          1
        );
      };
      const errored = deferredPromise();
      w.on('error', errored.resolve);
      w._write = () => {};
      r.pipe(w);
      // Removing some OTHER random listener should not do anything
      w.removeListener('error', () => {});
      removed = true;
      await errored.promise;
    }
    {
      const _err = new Error('this should be handled');
      const destination = new PassThrough();
      const errored = deferredPromise();
      destination.once(
        'error',
        (err) => {
          strictEqual(err, _err);
          errored.resolve();
        }
      );
      const stream = new Stream();
      stream.pipe(destination);
      destination.destroy(_err);
      await errored.promise;
    }
  }
};

export const pipeCleanup = {
  async test(ctrl, env, ctx) {
    function Writable() {
      this.writable = true;
      this.endCalls = 0;
      Stream.call(this);
    }
    Object.setPrototypeOf(Writable.prototype, Stream.prototype);
    Object.setPrototypeOf(Writable, Stream);
    Writable.prototype.end = function () {
      this.endCalls++;
    };
    Writable.prototype.destroy = function () {
      this.endCalls++;
    };
    function Readable() {
      this.readable = true;
      Stream.call(this);
    }
    Object.setPrototypeOf(Readable.prototype, Stream.prototype);
    Object.setPrototypeOf(Readable, Stream);
    function Duplex() {
      this.readable = true;
      Writable.call(this);
    }
    Object.setPrototypeOf(Duplex.prototype, Writable.prototype);
    Object.setPrototypeOf(Duplex, Writable);
    let i = 0;
    const limit = 100;
    let w = new Writable();
    let r;
    for (i = 0; i < limit; i++) {
      r = new Readable();
      r.pipe(w);
      r.emit('end');
    }
    strictEqual(r.listeners('end').length, 0);
    strictEqual(w.endCalls, limit);
    w.endCalls = 0;
    for (i = 0; i < limit; i++) {
      r = new Readable();
      r.pipe(w);
      r.emit('close');
    }
    strictEqual(r.listeners('close').length, 0);
    strictEqual(w.endCalls, limit);
    w.endCalls = 0;
    r = new Readable();
    for (i = 0; i < limit; i++) {
      w = new Writable();
      r.pipe(w);
      w.emit('close');
    }
    strictEqual(w.listeners('close').length, 0);
    r = new Readable();
    w = new Writable();
    const d = new Duplex();
    r.pipe(d); // pipeline A
    d.pipe(w); // pipeline B
    strictEqual(r.listeners('end').length, 2); // A.onend, A.cleanup
    strictEqual(r.listeners('close').length, 2); // A.onclose, A.cleanup
    strictEqual(d.listeners('end').length, 2); // B.onend, B.cleanup
    // A.cleanup, B.onclose, B.cleanup
    strictEqual(d.listeners('close').length, 3);
    strictEqual(w.listeners('end').length, 0);
    strictEqual(w.listeners('close').length, 1); // B.cleanup

    r.emit('end');
    strictEqual(d.endCalls, 1);
    strictEqual(w.endCalls, 0);
    strictEqual(r.listeners('end').length, 0);
    strictEqual(r.listeners('close').length, 0);
    strictEqual(d.listeners('end').length, 2); // B.onend, B.cleanup
    strictEqual(d.listeners('close').length, 2); // B.onclose, B.cleanup
    strictEqual(w.listeners('end').length, 0);
    strictEqual(w.listeners('close').length, 1); // B.cleanup

    d.emit('end');
    strictEqual(d.endCalls, 1);
    strictEqual(w.endCalls, 1);
    strictEqual(r.listeners('end').length, 0);
    strictEqual(r.listeners('close').length, 0);
    strictEqual(d.listeners('end').length, 0);
    strictEqual(d.listeners('close').length, 0);
    strictEqual(w.listeners('end').length, 0);
    strictEqual(w.listeners('close').length, 0);
  }
};

export const pipeCleanupPause = {
  async test(ctrl, env, ctx) {
    const done = deferredPromise();
    const reader = new Readable();
    const writer1 = new Writable();
    const writer2 = new Writable();

    // 560000 is chosen here because it is larger than the (default) highWaterMark
    // and will cause `.write()` to return false
    // See: https://github.com/nodejs/node/issues/2323
    const buffer = Buffer.allocUnsafe(560000);
    reader._read = () => {};
    writer1._write = function (chunk, encoding, cb) {
      this.emit('chunk-received');
      cb();
    }
    writer1.once('chunk-received', function () {
      reader.unpipe(writer1);
      reader.pipe(writer2);
      reader.push(buffer);
      queueMicrotask(function () {
        reader.push(buffer);
        queueMicrotask(function () {
          reader.push(buffer);
          done.resolve();
        });
      });
    });
    writer2._write = function (chunk, encoding, cb) {
      cb();
    };
    reader.pipe(writer1);
    reader.push(buffer);
    await done.promise;
  }
};

export const writableAdapter = {
  async test(ctrl, env, ctx) {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    // toWeb
    {
      const p = deferredPromise();
      const w = new Writable({
        write(chunk, encoding, callback) {
          strictEqual(dec.decode(chunk), 'ok');
          p.resolve();
        }
      });
      const ws = Writable.toWeb(w);
      const writer = ws.getWriter();
      await Promise.all([
        writer.write(enc.encode('ok')),
        p.promise
      ]);
    }

    // fromWeb
    {
      const p = deferredPromise();
      const ws = new WritableStream({
        write(chunk) {
          strictEqual(dec.decode(chunk), 'ok');
          p.resolve();
        }
      });
      const w = Writable.fromWeb(ws);
      const p2 = deferredPromise();
      w.write(enc.encode('ok'), p2.resolve);
      await Promise.all([p.promise, p2.promise]);
    }
  }
};

export const readableAdapter = {
  async test(ctrl, env, ctx) {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    // toWeb
    {
      const r = new Readable({
        read() {
          this.push(enc.encode('ok'));
        }
      });
      const rs = Readable.toWeb(r);
      const reader = rs.getReader();
      const value = await reader.read();
      strictEqual(dec.decode(value.value), 'ok');
    }

    {
      // Using a Node.js stream to feed into a Response...
      const r = new Readable({
        highWaterMark: 2,
        read() {}
      });
      setTimeout(() => r.push(enc.encode('ok')), 10);
      setTimeout(() => r.push(enc.encode(' there')), 20);
      setTimeout(() => r.push(null), 30);
      const rs = Readable.toWeb(r);
      const res = new Response(rs);
      const text = await res.text();
      strictEqual(text, 'ok there');
    }

    // fromWeb
    {
      const rs = new ReadableStream({
        pull(c) {
          c.enqueue(enc.encode('ok'));
          c.close();
        }
      });
      const r = Readable.fromWeb(rs);
      const p = deferredPromise();
      r.on('data', (chunk) => {
        strictEqual(dec.decode(chunk), 'ok');
        p.resolve();
      });
      await p.promise;
    }
  }
};
