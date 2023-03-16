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
  pipeline,
} from 'node:stream';

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

    // TODO(later): We do not yet implement fromWeb/toWeb
    // // Duplex.fromWeb
    // {
    //   const dataToRead = Buffer.from('hello');
    //   const dataToWrite = Buffer.from('world');

    //   const readable = new ReadableStream({
    //     start(controller) {
    //       controller.enqueue(dataToRead);
    //     },
    //   });

    //   const writable = new WritableStream({
    //     write: common.mustCall((chunk) => {
    //       assert.strictEqual(chunk, dataToWrite);
    //     })
    //   });

    //   const pair = { readable, writable };
    //   const duplex = Duplex.fromWeb(pair);

    //   duplex.write(dataToWrite);
    //   duplex.once('data', common.mustCall((chunk) => {
    //     assert.strictEqual(chunk, dataToRead);
    //   }));
    // }
    //
    // // Duplex.fromWeb - using utf8 and objectMode
    // {
    //   const dataToRead = 'hello';
    //   const dataToWrite = 'world';

    //   const readable = new ReadableStream({
    //     start(controller) {
    //       controller.enqueue(dataToRead);
    //     },
    //   });

    //   const writable = new WritableStream({
    //     write: common.mustCall((chunk) => {
    //       assert.strictEqual(chunk, dataToWrite);
    //     })
    //   });

    //   const pair = {
    //     readable,
    //     writable
    //   };
    //   const duplex = Duplex.fromWeb(pair, { encoding: 'utf8', objectMode: true });

    //   duplex.write(dataToWrite);
    //   duplex.once('data', common.mustCall((chunk) => {
    //     assert.strictEqual(chunk, dataToRead);
    //   }));
    // }

    // // Duplex.toWeb
    // {
    //   const dataToRead = Buffer.from('hello');
    //   const dataToWrite = Buffer.from('world');

    //   const duplex = Duplex({
    //     read() {
    //       this.push(dataToRead);
    //       this.push(null);
    //     },
    //     write: common.mustCall((chunk) => {
    //       assert.strictEqual(chunk, dataToWrite);
    //     })
    //   });

    //   const { writable, readable } = Duplex.toWeb(duplex);
    //   writable.getWriter().write(dataToWrite);

    //   readable.getReader().read().then(common.mustCall((result) => {
    //     assert.deepStrictEqual(Buffer.from(result.value), dataToRead);
    //   }));
    // }
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
