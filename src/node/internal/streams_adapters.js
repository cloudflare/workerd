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

/* todo: the following is adopted code, enabling linting one day */
/* eslint-disable */
import { Readable } from 'node-internal:streams_readable';

import { Writable } from 'node-internal:streams_writable';

import { Duplex } from 'node-internal:streams_duplex';

import {
  destroy,
  eos as finished,
  isDestroyed,
  isReadable,
  isWritable,
  isWritableEnded,
} from 'node-internal:streams_util';

import { Buffer } from 'node-internal:internal_buffer';

import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_STREAM_PREMATURE_CLOSE,
  AbortError,
} from 'node-internal:internal_errors';

import {
  createDeferredPromise,
  normalizeEncoding,
} from 'node-internal:internal_utils';

import { validateBoolean, validateObject } from 'node-internal:validators';

import * as process from 'node-internal:process';

const encoder = new TextEncoder();

/**
 * @param {Writable} streamWritable
 * @returns {WritableStream}
 */
export function newWritableStreamFromStreamWritable(streamWritable) {
  // Not using the internal/streams/utils isWritableNodeStream utility
  // here because it will return false if streamWritable is a Duplex
  // whose writable option is false. For a Duplex that is not writable,
  // we want it to pass this check but return a closed WritableStream.
  // We check if the given stream is a stream.Writable or http.OutgoingMessage
  const checkIfWritableOrOutgoingMessage =
    streamWritable &&
    typeof streamWritable?.write === 'function' &&
    typeof streamWritable?.on === 'function';
  if (!checkIfWritableOrOutgoingMessage) {
    throw new ERR_INVALID_ARG_TYPE(
      'streamWritable',
      'stream.Writable',
      streamWritable
    );
  }

  if (isDestroyed(streamWritable) || !isWritable(streamWritable)) {
    const writable = new WritableStream();
    writable.close();
    return writable;
  }

  const highWaterMark = streamWritable.writableHighWaterMark;
  const strategy = streamWritable.writableObjectMode
    ? new CountQueuingStrategy({ highWaterMark })
    : { highWaterMark };

  let controller;
  let backpressurePromise;
  let closed;

  function onDrain() {
    if (backpressurePromise !== undefined) backpressurePromise.resolve();
  }

  const cleanup = finished(streamWritable, (error) => {
    if (error?.code === 'ERR_STREAM_PREMATURE_CLOSE') {
      const err = new AbortError(undefined, { cause: error });
      error = err;
    }

    cleanup();
    // This is a protection against non-standard, legacy streams
    // that happen to emit an error event again after finished is called.
    streamWritable.on('error', () => {});
    if (error != null) {
      if (backpressurePromise !== undefined) backpressurePromise.reject(error);
      // If closed is not undefined, the error is happening
      // after the WritableStream close has already started.
      // We need to reject it here.
      if (closed !== undefined) {
        closed.reject(error);
        closed = undefined;
      }
      controller.error(error);
      controller = undefined;
      return;
    }

    if (closed !== undefined) {
      closed.resolve();
      closed = undefined;
      return;
    }
    controller.error(new AbortError());
    controller = undefined;
  });

  streamWritable.on('drain', onDrain);

  return new WritableStream(
    {
      start(c) {
        controller = c;
      },

      async write(chunk) {
        if (streamWritable.writableNeedDrain || !streamWritable.write(chunk)) {
          backpressurePromise = createDeferredPromise();
          return backpressurePromise.promise.finally(() => {
            backpressurePromise = undefined;
          });
        }
      },

      abort(reason) {
        destroy(streamWritable, reason);
      },

      close() {
        if (closed === undefined && !isWritableEnded(streamWritable)) {
          closed = createDeferredPromise();
          streamWritable.end();
          return closed.promise;
        }

        controller = undefined;
        return Promise.resolve();
      },
    },
    strategy
  );
}

/**
 * @param {WritableStream} writableStream
 * @param {{
 *   decodeStrings? : boolean,
 *   highWaterMark? : number,
 *   objectMode? : boolean,
 *   signal? : AbortSignal,
 * }} [options]
 * @returns {Writable}
 */
export function newStreamWritableFromWritableStream(
  writableStream,
  options = {}
) {
  if (!(writableStream instanceof WritableStream)) {
    throw new ERR_INVALID_ARG_TYPE(
      'writableStream',
      'WritableStream',
      writableStream
    );
  }

  validateObject(options, 'options');
  const {
    highWaterMark,
    decodeStrings = true,
    objectMode = false,
    signal,
  } = options;

  validateBoolean(objectMode, 'options.objectMode');
  validateBoolean(decodeStrings, 'options.decodeStrings');

  const writer = writableStream.getWriter();
  let closed = false;

  const writable = new Writable({
    highWaterMark,
    objectMode,
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
          process.nextTick(() => destroy(writable, error));
        }
      }

      writer.ready.then(() => {
        return Promise.all(chunks.map((data) => writer.write(data))).then(
          done,
          done
        );
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
          destroy(writable, error);
        }
      }

      writer.ready.then(() => {
        return writer.write(chunk).then(done, done);
      }, done);
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
          process.nextTick(() => {
            throw error;
          });
        }
      }

      if (!closed) {
        if (error != null) {
          writer.abort(error).then(done, done);
        } else {
          writer.close().then(done, done);
        }
        return;
      }

      done();
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
          process.nextTick(() => destroy(writable, error));
        }
      }

      if (!closed) {
        writer.close().then(done, done);
      }
    },
  });

  writer.closed.then(
    () => {
      // If the WritableStream closes before the stream.Writable has been
      // ended, we signal an error on the stream.Writable.
      closed = true;
      if (!isWritableEnded(writable))
        destroy(writable, new ERR_STREAM_PREMATURE_CLOSE());
    },
    (error) => {
      // If the WritableStream errors before the stream.Writable has been
      // destroyed, signal an error on the stream.Writable.
      closed = true;
      destroy(writable, error);
    }
  );

  return writable;
}

/**
 * @typedef {import('./queuingstrategies').QueuingStrategy} QueuingStrategy
 * @param {Readable} streamReadable
 * @param {{
 *  strategy : QueuingStrategy
 * }} [options]
 * @returns {ReadableStream}
 */
export function newReadableStreamFromStreamReadable(
  streamReadable,
  options = {}
) {
  // Not using the internal/streams/utils isReadableNodeStream utility
  // here because it will return false if streamReadable is a Duplex
  // whose readable option is false. For a Duplex that is not readable,
  // we want it to pass this check but return a closed ReadableStream.
  if (typeof streamReadable?._readableState !== 'object') {
    throw new ERR_INVALID_ARG_TYPE(
      'streamReadable',
      'stream.Readable',
      streamReadable
    );
  }

  if (isDestroyed(streamReadable) || !isReadable(streamReadable)) {
    const readable = new ReadableStream();
    readable.cancel();
    return readable;
  }

  const objectMode = streamReadable.readableObjectMode;
  const highWaterMark = streamReadable.readableHighWaterMark;

  const evaluateStrategyOrFallback = (strategy) => {
    // If there is a strategy available, use it
    if (strategy) return strategy;

    if (objectMode) {
      // When running in objectMode explicitly but no strategy, we just fall
      // back to CountQueuingStrategy
      return new CountQueuingStrategy({ highWaterMark });
    }

    // When not running in objectMode explicitly, we just fall
    // back to a minimal strategy that just specifies the highWaterMark
    // and no size algorithm. Using a ByteLengthQueuingStrategy here
    // is unnecessary.
    return { highWaterMark };
  };

  const strategy = evaluateStrategyOrFallback(options?.strategy);

  let controller;

  function onData(chunk) {
    // Copy the Buffer to detach it from the pool.
    if (Buffer.isBuffer(chunk) && !objectMode) chunk = new Uint8Array(chunk);
    controller.enqueue(chunk);
    if (controller.desiredSize <= 0) streamReadable.pause();
  }

  streamReadable.pause();

  const cleanup = finished(streamReadable, (error) => {
    if (error?.code === 'ERR_STREAM_PREMATURE_CLOSE') {
      const err = new AbortError(undefined, { cause: error });
      error = err;
    }

    cleanup();
    // This is a protection against non-standard, legacy streams
    // that happen to emit an error event again after finished is called.
    streamReadable.on('error', () => {});
    if (error) return controller.error(error);
    controller.close();
  });

  streamReadable.on('data', onData);

  return new ReadableStream(
    {
      start(c) {
        controller = c;
      },

      pull() {
        streamReadable.resume();
      },

      cancel(reason) {
        destroy(streamReadable, reason);
      },
    },
    strategy
  );
}

/**
 * @param {ReadableStream} readableStream
 * @param {{
 *   highWaterMark? : number,
 *   encoding? : string,
 *   objectMode? : boolean,
 *   signal? : AbortSignal,
 * }} [options]
 * @returns {Readable}
 */
export function newStreamReadableFromReadableStream(
  readableStream,
  options = {}
) {
  if (!(readableStream instanceof ReadableStream)) {
    throw new ERR_INVALID_ARG_TYPE(
      'readableStream',
      'ReadableStream',
      readableStream
    );
  }

  validateObject(options, 'options');
  const { highWaterMark, encoding, objectMode = false, signal } = options;

  if (encoding !== undefined && !Buffer.isEncoding(encoding))
    throw new ERR_INVALID_ARG_VALUE(encoding, 'options.encoding');
  validateBoolean(objectMode, 'options.objectMode');

  const reader = readableStream.getReader();
  let closed = false;

  const readable = new Readable({
    objectMode,
    highWaterMark,
    encoding,
    signal,

    read() {
      reader.read().then(
        (chunk) => {
          if (chunk.done) {
            // Value should always be undefined here.
            readable.push(null);
          } else {
            readable.push(chunk.value);
          }
        },
        (error) => destroy(readable, error)
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
          process.nextTick(() => {
            throw error;
          });
        }
      }

      if (!closed) {
        reader.cancel(error).then(done, done);
        return;
      }
      done();
    },
  });

  reader.closed.then(
    () => {
      closed = true;
    },
    (error) => {
      closed = true;
      destroy(readable, error);
    }
  );

  return readable;
}

/**
 * @typedef {import('./readablestream').ReadableWritablePair
 * } ReadableWritablePair
 * @typedef {import('../../stream').Duplex} Duplex
 */

/**
 * @param {Duplex} duplex
 * @returns {ReadableWritablePair}
 */
export function newReadableWritablePairFromDuplex(duplex) {
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

  const readable = isReadable(duplex)
    ? newReadableStreamFromStreamReadable(duplex)
    : new ReadableStream();

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
          process.nextTick(() => destroy(duplex, error));
        }
      }

      writer.ready.then(() => {
        return Promise.all(
          chunks.map((data) => {
            return writer.write(data);
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
          destroy(duplex, error);
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
          process.nextTick(() => destroy(duplex, error));
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
        (error) => destroy(duplex, error)
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
          process.nextTick(() => {
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
        destroy(duplex, new ERR_STREAM_PREMATURE_CLOSE());
    },
    (error) => {
      writableClosed = true;
      readableClosed = true;
      destroy(duplex, error);
    }
  );

  reader.closed.then(
    () => {
      readableClosed = true;
    },
    (error) => {
      writableClosed = true;
      readableClosed = true;
      destroy(duplex, error);
    }
  );

  return duplex;
}
