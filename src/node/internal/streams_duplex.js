// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

/* TODO: the following is adopted code, enabling linting one day */
/* eslint-disable */

import { Buffer } from 'node-internal:internal_buffer';
import {
  Readable,
  newReadableStreamFromStreamReadable,
} from 'node-internal:streams_readable';
import {
  Writable,
  newWritableStreamFromStreamWritable,
} from 'node-internal:streams_writable';
import { ok as assert } from 'node-internal:internal_assert';
import { Stream } from 'node-internal:streams_legacy';
import { nextTick } from 'node-internal:internal_process';
import { validateBoolean, validateObject } from 'node-internal:validators';
import { normalizeEncoding } from 'node-internal:internal_utils';
import { addAbortSignal } from 'node-internal:streams_add_abort_signal';

import {
  isDestroyed,
  isReadable,
  isWritable,
  isIterable,
  isNodeStream,
  isWritableEnded,
  isReadableNodeStream,
  isWritableNodeStream,
  isDuplexNodeStream,
  kOnConstructed,
} from 'node-internal:streams_util';
import {
  construct as destroyConstruct,
  destroyer,
} from 'node-internal:streams_destroy';
import { eos } from 'node-internal:streams_end_of_stream';

import {
  AbortError,
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_INVALID_RETURN_VALUE,
  ERR_STREAM_PREMATURE_CLOSE,
} from 'node-internal:internal_errors';

/**
 * @typedef {import('./readablestream').ReadableWritablePair
 * } ReadableWritablePair
 * @typedef {import('../../stream').Duplex} Duplex
 */
const encoder = new TextEncoder();

Object.setPrototypeOf(Duplex.prototype, Readable.prototype);
Object.setPrototypeOf(Duplex, Readable);
{
  const keys = Object.keys(Writable.prototype);
  // Allow the keys array to be GC'ed.
  for (let i = 0; i < keys.length; i++) {
    const method = keys[i];
    Duplex.prototype[method] ||= Writable.prototype[method];
  }
}

// Use the `destroy` method of `Writable`.
Duplex.prototype.destroy = Writable.prototype.destroy;

export function Duplex(options) {
  if (!(this instanceof Duplex)) return new Duplex(options);

  this._events ??= {
    close: undefined,
    error: undefined,
    prefinish: undefined,
    finish: undefined,
    drain: undefined,
    data: undefined,
    end: undefined,
    readable: undefined,
    // Skip uncommon events...
    // pause: undefined,
    // resume: undefined,
    // pipe: undefined,
    // unpipe: undefined,
    // [destroyImpl.kConstruct]: undefined,
    // [destroyImpl.kDestroy]: undefined,
  };

  this._readableState = new Readable.ReadableState(options, this, true);
  this._writableState = new Writable.WritableState(options, this, true);

  if (options) {
    this.allowHalfOpen = options.allowHalfOpen !== false;

    if (options.readable === false) {
      this._readableState.readable = false;
      this._readableState.ended = true;
      this._readableState.endEmitted = true;
    }

    if (options.writable === false) {
      this._writableState.writable = false;
      this._writableState.ending = true;
      this._writableState.ended = true;
      this._writableState.finished = true;
    }

    if (typeof options.read === 'function') this._read = options.read;

    if (typeof options.write === 'function') this._write = options.write;

    if (typeof options.writev === 'function') this._writev = options.writev;

    if (typeof options.destroy === 'function') this._destroy = options.destroy;

    if (typeof options.final === 'function') this._final = options.final;

    if (typeof options.construct === 'function')
      this._construct = options.construct;

    if (options.signal) {
      addAbortSignal(options.signal, this);
    }
  } else {
    this.allowHalfOpen = true;
  }

  Stream.call(this, options);

  if (this._construct != null) {
    destroyConstruct(this, () => {
      this._readableState[kOnConstructed](this);
      this._writableState[kOnConstructed](this);
    });
  }
}

// Use the `destroy` method of `Writable`.
Duplex.prototype.destroy = Writable.prototype.destroy;

Object.defineProperties(Duplex.prototype, {
  writable: {
    __proto__: null,
    ...Object.getOwnPropertyDescriptor(Writable.prototype, 'writable'),
  },
  writableHighWaterMark: {
    __proto__: null,
    ...Object.getOwnPropertyDescriptor(
      Writable.prototype,
      'writableHighWaterMark'
    ),
  },
  writableObjectMode: {
    __proto__: null,
    ...Object.getOwnPropertyDescriptor(
      Writable.prototype,
      'writableObjectMode'
    ),
  },
  writableBuffer: {
    __proto__: null,
    ...Object.getOwnPropertyDescriptor(Writable.prototype, 'writableBuffer'),
  },
  writableLength: {
    __proto__: null,
    ...Object.getOwnPropertyDescriptor(Writable.prototype, 'writableLength'),
  },
  writableFinished: {
    __proto__: null,
    ...Object.getOwnPropertyDescriptor(Writable.prototype, 'writableFinished'),
  },
  writableCorked: {
    __proto__: null,
    ...Object.getOwnPropertyDescriptor(Writable.prototype, 'writableCorked'),
  },
  writableEnded: {
    __proto__: null,
    ...Object.getOwnPropertyDescriptor(Writable.prototype, 'writableEnded'),
  },
  writableNeedDrain: {
    __proto__: null,
    ...Object.getOwnPropertyDescriptor(Writable.prototype, 'writableNeedDrain'),
  },

  destroyed: {
    __proto__: null,
    get() {
      if (
        this._readableState === undefined ||
        this._writableState === undefined
      ) {
        return false;
      }
      return this._readableState.destroyed && this._writableState.destroyed;
    },
    set(value) {
      // Backward compatibility, the user is explicitly
      // managing destroyed.
      if (this._readableState && this._writableState) {
        this._readableState.destroyed = value;
        this._writableState.destroyed = value;
      }
    },
  },
});

export function fromWeb(pair, options) {
  return newStreamDuplexFromReadableWritablePair(pair, options);
}

export function toWeb(duplex) {
  return newReadableWritablePairFromDuplex(duplex);
}

export function toBYOBWeb(duplex) {
  return newReadableWritablePairFromDuplex(duplex, true /* createTypeBytes */);
}

export function from(body) {
  return duplexify(body, 'body');
}

Duplex.fromWeb = fromWeb;
Duplex.toWeb = toWeb;
Duplex.from = from;

// ======================================================================================

function isBlob(b) {
  return b instanceof Blob;
}

// This is needed for pre node 17.
class Duplexify extends Duplex {
  constructor(options) {
    super(options);
    // https://github.com/nodejs/node/pull/34385

    if (
      (options === null || options === undefined
        ? undefined
        : options.readable) === false
    ) {
      this['_readableState'].readable = false;
      this['_readableState'].ended = true;
      this['_readableState'].endEmitted = true;
    }
    if (
      (options === null || options === undefined
        ? undefined
        : options.writable) === false
    ) {
      this['_readableState'].writable = false;
      this['_readableState'].ending = true;
      this['_readableState'].ended = true;
      this['_readableState'].finished = true;
    }
  }
}

function duplexify(body, name) {
  if (isDuplexNodeStream(body)) {
    return body;
  }
  if (isReadableNodeStream(body)) {
    return _duplexify({
      readable: body,
    });
  }
  if (isWritableNodeStream(body)) {
    return _duplexify({
      writable: body,
    });
  }
  if (isNodeStream(body)) {
    return _duplexify({
      writable: false,
      readable: false,
    });
  }

  if (body instanceof ReadableStream) {
    return _duplexify({ readable: Readable.fromWeb(body) });
  }

  if (body instanceof WritableStream) {
    return _duplexify({ writable: Writable.fromWeb(body) });
  }

  if (typeof body === 'function') {
    const { value, write, final, destroy } = fromAsyncGen(body);
    if (isIterable(value)) {
      return Readable.from(Duplexify, value, {
        // TODO (ronag): highWaterMark?
        objectMode: true,
        write,
        final,
        destroy,
      });
    }
    const then = value.then;
    if (typeof then === 'function') {
      let d;
      const promise = Reflect.apply(then, value, [
        (val) => {
          if (val != null) {
            throw new ERR_INVALID_RETURN_VALUE('nully', 'body', val);
          }
        },
        (err) => {
          destroyer(d, err);
        },
      ]);

      return (d = new Duplexify({
        // TODO (ronag): highWaterMark?
        objectMode: true,
        readable: false,
        write,
        final(cb) {
          final(async () => {
            try {
              await promise;
              nextTick(cb, null);
            } catch (err) {
              nextTick(cb, err);
            }
          });
        },
        destroy,
      }));
    }
    throw new ERR_INVALID_RETURN_VALUE(
      'Iterable, AsyncIterable or AsyncFunction',
      name,
      value
    );
  }
  if (isBlob(body)) {
    return duplexify(body.arrayBuffer(), name);
  }
  if (isIterable(body)) {
    return Readable.from(Duplexify, body, {
      // TODO (ronag): highWaterMark?
      objectMode: true,
      writable: false,
    });
  }

  if (
    body?.readable instanceof ReadableStream &&
    body?.writable instanceof WritableStream
  ) {
    return Duplexify.fromWeb(body);
  }

  if (
    typeof (body === null || body === undefined ? undefined : body.writable) ===
      'object' ||
    typeof (body === null || body === undefined ? undefined : body.readable) ===
      'object'
  ) {
    const readable =
      body !== null && body !== undefined && body.readable
        ? isReadableNodeStream(
            body === null || body === undefined ? undefined : body.readable
          )
          ? body === null || body === undefined
            ? undefined
            : body.readable
          : duplexify(body.readable, name)
        : undefined;
    const writable =
      body !== null && body !== undefined && body.writable
        ? isWritableNodeStream(
            body === null || body === undefined ? undefined : body.writable
          )
          ? body === null || body === undefined
            ? undefined
            : body.writable
          : duplexify(body.writable, name)
        : undefined;
    return _duplexify({
      readable,
      writable,
    });
  }
  const then = body?.then;
  if (typeof then === 'function') {
    let d;
    Reflect.apply(then, body, [
      (val) => {
        if (val != null) {
          d.push(val);
        }
        d.push(null);
      },
      (err) => {
        destroyer(d, err);
      },
    ]);

    return (d = new Duplexify({
      objectMode: true,
      writable: false,
      read() {},
    }));
  }
  throw new ERR_INVALID_ARG_TYPE(
    name,
    [
      'Blob',
      'ReadableStream',
      'WritableStream',
      'Stream',
      'Iterable',
      'AsyncIterable',
      'Function',
      '{ readable, writable } pair',
      'Promise',
    ],
    body
  );
}

function fromAsyncGen(fn) {
  let { promise, resolve } = Promise.withResolvers();
  const ac = new AbortController();
  const signal = ac.signal;
  const value = fn(
    (async function* () {
      while (true) {
        const _promise = promise;
        promise = null;
        const { chunk, done, cb } = await _promise;
        nextTick(cb);
        if (done) return;
        if (signal.aborted)
          throw new AbortError(undefined, {
            cause: signal.reason,
          });
        ({ promise, resolve } = Promise.withResolvers());
        yield chunk;
      }
    })(),
    {
      signal,
    }
  );
  return {
    value,
    write(chunk, _encoding, cb) {
      const _resolve = resolve;
      resolve = null;
      _resolve({
        chunk,
        done: false,
        cb,
      });
    },
    final(cb) {
      const _resolve = resolve;
      resolve = null;
      _resolve({
        done: true,
        cb,
      });
    },
    destroy(err, cb) {
      ac.abort();
      cb(err);
    },
  };
}

function _duplexify(pair) {
  const r =
    pair.readable && typeof pair.readable.read !== 'function'
      ? Readable.wrap(pair.readable)
      : pair.readable;
  const w = pair.writable;
  let readable = !!isReadable(r);
  let writable = !!isWritable(w);
  let ondrain;
  let onfinish;
  let onreadable;
  let onclose;
  let d;
  function onfinished(err) {
    const cb = onclose;
    onclose = null;
    if (cb) {
      cb(err);
    } else if (err) {
      d.destroy(err);
    } else if (!readable && !writable) {
      d.destroy();
    }
  }

  // TODO(ronag): Avoid double buffering.
  // Implement Writable/Readable/Duplex traits.
  // See, https://github.com/nodejs/node/pull/33515.
  d = new Duplexify({
    // TODO (ronag): highWaterMark?
    readableObjectMode: !!(
      r !== null &&
      r !== undefined &&
      r.readableObjectMode
    ),
    writableObjectMode: !!(
      w !== null &&
      w !== undefined &&
      w.writableObjectMode
    ),
    readable,
    writable,
  });
  if (writable) {
    eos(w, (err) => {
      writable = false;
      if (err) {
        destroyer(r, err);
      }
      onfinished(err);
    });
    d._write = function (chunk, encoding, callback) {
      if (w.write(chunk, encoding)) {
        callback();
      } else {
        ondrain = callback;
      }
    };
    d._final = function (callback) {
      w.end();
      onfinish = callback;
    };
    w.on('drain', function () {
      if (ondrain) {
        const cb = ondrain;
        ondrain = null;
        cb();
      }
    });
    w.on('finish', function () {
      if (onfinish) {
        const cb = onfinish;
        onfinish = null;
        cb();
      }
    });
  }
  if (readable) {
    eos(r, (err) => {
      readable = false;
      if (err) {
        destroyer(r, err);
      }
      onfinished(err);
    });
    r.on('readable', function () {
      if (onreadable) {
        const cb = onreadable;
        onreadable = null;
        cb();
      }
    });
    r.on('end', function () {
      d.push(null);
    });
    d._read = function () {
      while (true) {
        const buf = r.read();
        if (buf === null) {
          onreadable = d._read;
          return;
        }
        if (!d.push(buf)) {
          return;
        }
      }
    };
  }
  d._destroy = function (err, callback) {
    if (!err && onclose !== null) {
      err = new AbortError();
    }
    onreadable = null;
    ondrain = null;
    onfinish = null;
    if (onclose === null) {
      callback(err);
    } else {
      onclose = callback;
      destroyer(w, err);
      destroyer(r, err);
    }
  };
  return d;
}

const kCallback = Symbol('Callback');
const kInitOtherSide = Symbol('InitOtherSide');

class DuplexSide extends Duplex {
  #otherSide = null;

  constructor(options) {
    super(options);
    this[kCallback] = null;
    this.#otherSide = null;
  }

  [kInitOtherSide](otherSide) {
    // Ensure this can only be set once, to enforce encapsulation.
    if (this.#otherSide === null) {
      this.#otherSide = otherSide;
    } else {
      assert(this.#otherSide === null);
    }
  }

  _read() {
    const callback = this[kCallback];
    if (callback) {
      this[kCallback] = null;
      callback();
    }
  }

  _write(chunk, encoding, callback) {
    assert(this.#otherSide !== null);
    assert(this.#otherSide[kCallback] === null);
    if (chunk.length === 0) {
      nextTick(callback);
    } else {
      this.#otherSide.push(chunk);
      this.#otherSide[kCallback] = callback;
    }
  }

  _final(callback) {
    this.#otherSide.on('end', callback);
    this.#otherSide.push(null);
  }
}

export function duplexPair(options) {
  const side0 = new DuplexSide(options);
  const side1 = new DuplexSide(options);
  side0[kInitOtherSide](side1);
  side1[kInitOtherSide](side0);
  return [side0, side1];
}

/**
 * @param {Duplex} duplex
 * @returns {ReadableWritablePair}
 */
export function newReadableWritablePairFromDuplex(
  duplex,
  createTypeBytes = false
) {
  // Not using the internal/streams/utils isWritableNodeStream and
  // isReadableNodeStream utilities here because they will return false
  // if the duplex was created with writable or readable options set to
  // false. Instead, we'll check the readable and writable state after
  // and return closed WritableStream or closed ReadableStream as
  // necessary.
  if (
    typeof duplex?._writableState !== 'object' ||
    typeof duplex?._readableState !== 'object'
  ) {
    throw new ERR_INVALID_ARG_TYPE('duplex', 'stream.Duplex', duplex);
  }

  if (isDestroyed(duplex)) {
    const writable = new WritableStream();
    const readable = new ReadableStream();
    writable.close();
    readable.cancel();
    return { readable, writable };
  }

  const writable = isWritable(duplex)
    ? newWritableStreamFromStreamWritable(duplex)
    : new WritableStream();

  if (!isWritable(duplex)) writable.close();

  const readableOptions = createTypeBytes ? { type: 'bytes' } : {};
  const readable = isReadable(duplex)
    ? newReadableStreamFromStreamReadable(duplex, {}, createTypeBytes)
    : new ReadableStream(readableOptions);

  if (!isReadable(duplex)) readable.cancel();

  return { writable, readable };
}

/**
 * @param {ReadableWritablePair} pair
 * @param {{
 *   allowHalfOpen? : boolean,
 *   decodeStrings? : boolean,
 *   encoding? : string,
 *   highWaterMark? : number,
 *   objectMode? : boolean,
 *   signal? : AbortSignal,
 * }} [options]
 * @returns {Duplex}
 */
export function newStreamDuplexFromReadableWritablePair(
  pair = {},
  options = {}
) {
  validateObject(pair, 'pair');
  const { readable: readableStream, writable: writableStream } = pair;

  if (!(readableStream instanceof ReadableStream)) {
    throw new ERR_INVALID_ARG_TYPE(
      'pair.readable',
      'ReadableStream',
      readableStream
    );
  }
  if (!(writableStream instanceof WritableStream)) {
    throw new ERR_INVALID_ARG_TYPE(
      'pair.writable',
      'WritableStream',
      writableStream
    );
  }

  validateObject(options, 'options');
  const {
    allowHalfOpen = false,
    objectMode = false,
    encoding,
    decodeStrings = true,
    highWaterMark,
    signal,
  } = options;

  validateBoolean(objectMode, 'options.objectMode');
  if (encoding !== undefined && !Buffer.isEncoding(encoding))
    throw new ERR_INVALID_ARG_VALUE(encoding, 'options.encoding');

  const writer = writableStream.getWriter();
  const reader = readableStream.getReader();
  let writableClosed = false;
  let readableClosed = false;

  const duplex = new Duplex({
    allowHalfOpen,
    highWaterMark,
    objectMode,
    encoding,
    decodeStrings,
    signal,

    writev(chunks, callback) {
      function done(error) {
        error = error.filter((e) => e);
        try {
          callback(error.length === 0 ? undefined : error);
        } catch (error) {
          // In a next tick because this is happening within
          // a promise context, and if there are any errors
          // thrown we don't want those to cause an unhandled
          // rejection. Let's just escape the promise and
          // handle it separately.
          nextTick(() => destroy.call(duplex, error));
        }
      }

      writer.ready.then(() => {
        return Promise.all(
          chunks.map((data) => {
            return writer.write(data.chunk);
          })
        ).then(done, done);
      }, done);
    },

    write(chunk, encoding, callback) {
      if (typeof chunk === 'string' && decodeStrings && !objectMode) {
        const enc = normalizeEncoding(encoding);

        if (enc === 'utf8') {
          chunk = encoder.encode(chunk);
        } else {
          chunk = Buffer.from(chunk, encoding);
          chunk = new Uint8Array(
            chunk.buffer,
            chunk.byteOffset,
            chunk.byteLength
          );
        }
      }

      function done(error) {
        try {
          callback(error);
        } catch (error) {
          destroy.call(duplex, error);
        }
      }

      writer.ready.then(() => {
        return writer.write(chunk).then(done, done);
      }, done);
    },

    final(callback) {
      function done(error) {
        try {
          callback(error);
        } catch (error) {
          // In a next tick because this is happening within
          // a promise context, and if there are any errors
          // thrown we don't want those to cause an unhandled
          // rejection. Let's just escape the promise and
          // handle it separately.
          nextTick(() => destroy.call(duplex, error));
        }
      }

      if (!writableClosed) {
        writer.close().then(done, done);
      }
    },

    read() {
      reader.read().then(
        (chunk) => {
          if (chunk.done) {
            duplex.push(null);
          } else {
            duplex.push(chunk.value);
          }
        },
        (error) => destroy.call(duplex, error)
      );
    },

    destroy(error, callback) {
      function done() {
        try {
          callback(error);
        } catch (error) {
          // In a next tick because this is happening within
          // a promise context, and if there are any errors
          // thrown we don't want those to cause an unhandled
          // rejection. Let's just escape the promise and
          // handle it separately.
          nextTick(() => {
            throw error;
          });
        }
      }

      async function closeWriter() {
        if (!writableClosed) await writer.abort(error);
      }

      async function closeReader() {
        if (!readableClosed) await reader.cancel(error);
      }

      if (!writableClosed || !readableClosed) {
        Promise.all([closeWriter(), closeReader()]).then(done, done);
        return;
      }

      done();
    },
  });

  writer.closed.then(
    () => {
      writableClosed = true;
      if (!isWritableEnded(duplex))
        destroy.call(duplex, new ERR_STREAM_PREMATURE_CLOSE());
    },
    (error) => {
      writableClosed = true;
      readableClosed = true;
      destroy.call(duplex, error);
    }
  );

  reader.closed.then(
    () => {
      readableClosed = true;
    },
    (error) => {
      writableClosed = true;
      readableClosed = true;
      destroy.call(duplex, error);
    }
  );

  return duplex;
}
