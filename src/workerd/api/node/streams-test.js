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
  Transform,
  PassThrough,
  addAbortSignal,
  destroy,
  finished,
  pipeline,
} from 'node:stream';

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
          assert(false, chunk);
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
