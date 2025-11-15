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

import { EventEmitter } from 'node-internal:events';
import * as destroyImpl from 'node-internal:streams_destroy';
import { Stream } from 'node-internal:streams_legacy';
import { Buffer } from 'node-internal:internal_buffer';
import { nextTick } from 'node-internal:internal_process';
import { normalizeEncoding } from 'node-internal:internal_utils';
import { validateBoolean, validateObject } from 'node-internal:validators';
import {
  kState,
  // bitfields
  kObjectMode,
  kErrorEmitted,
  kAutoDestroy,
  kEmitClose,
  kDestroyed,
  kClosed,
  kCloseEmitted,
  kErrored,
  kConstructed,
  kOnConstructed,
  isDestroyed,
  isWritable,
  isWritableEnded,
  isWritableStream,
  handleKnownInternalErrors,
} from 'node-internal:streams_util';
import { finished, eos, nop } from 'node-internal:streams_end_of_stream';
import { addAbortSignal } from 'node-internal:streams_add_abort_signal';
import {
  getHighWaterMark,
  getDefaultHighWaterMark,
} from 'node-internal:streams_state';

import {
  AbortError,
  ERR_INVALID_ARG_TYPE,
  ERR_METHOD_NOT_IMPLEMENTED,
  ERR_MULTIPLE_CALLBACK,
  ERR_STREAM_CANNOT_PIPE,
  ERR_STREAM_DESTROYED,
  ERR_STREAM_ALREADY_FINISHED,
  ERR_STREAM_NULL_VALUES,
  ERR_STREAM_WRITE_AFTER_END,
  ERR_UNKNOWN_ENCODING,
  ERR_STREAM_PREMATURE_CLOSE,
} from 'node-internal:internal_errors';

const streamsNodejsV24Compat =
  Cloudflare.compatibilityFlags.enable_streams_nodejs_v24_compat; // eslint-disable-line no-undef

const encoder = new globalThis.TextEncoder();

const kOnFinishedValue = Symbol('kOnFinishedValue');
const kErroredValue = Symbol('kErroredValue');
const kDefaultEncodingValue = Symbol('kDefaultEncodingValue');
const kWriteCbValue = Symbol('kWriteCbValue');
const kAfterWriteTickInfoValue = Symbol('kAfterWriteTickInfoValue');
const kBufferedValue = Symbol('kBufferedValue');

// Bitfield flag constants for WritableState. Each constant uses left-shift (<<) to set a specific
// bit position, allowing multiple boolean flags to be stored efficiently in a single integer (kState).
// For example, `1 << 9` creates a value with only bit 9 set (value: 512).
const kSync = 1 << 9;
const kFinalCalled = 1 << 10;
const kNeedDrain = 1 << 11;
const kEnding = 1 << 12;
const kFinished = 1 << 13;
const kDecodeStrings = 1 << 14;
const kWriting = 1 << 15;
const kBufferProcessing = 1 << 16;
const kPrefinished = 1 << 17;
const kAllBuffers = 1 << 18;
const kAllNoop = 1 << 19;
const kOnFinished = 1 << 20;
const kHasWritable = 1 << 21;
const kWritable = 1 << 22;
const kCorked = 1 << 23;
const kDefaultUTF8Encoding = 1 << 24;
const kWriteCb = 1 << 25;
const kExpectWriteCb = 1 << 26;
const kAfterWriteTickInfo = 1 << 27;
const kAfterWritePending = 1 << 28;
const kBuffered = 1 << 29;
const kEnded = 1 << 30;

// TODO(benjamingr) it is likely slower to do it this way than with free functions
function makeBitMapDescriptor(bit) {
  return {
    // This is not a breaking change according to Node.js but better safe than sorry.
    // Ref: https://github.com/nodejs/node/pull/49834
    enumerable: !streamsNodejsV24Compat,
    get() {
      return (this[kState] & bit) !== 0;
    },
    set(value) {
      if (value) this[kState] |= bit;
      else this[kState] &= ~bit;
    },
  };
}
Object.defineProperties(WritableState.prototype, {
  // Object stream flag to indicate whether or not this stream
  // contains buffers or objects.
  objectMode: makeBitMapDescriptor(kObjectMode),

  // if _final has been called.
  finalCalled: makeBitMapDescriptor(kFinalCalled),

  // drain event flag.
  needDrain: makeBitMapDescriptor(kNeedDrain),

  // At the start of calling end()
  ending: makeBitMapDescriptor(kEnding),

  // When end() has been called, and returned.
  ended: makeBitMapDescriptor(kEnded),

  // When 'finish' is emitted.
  finished: makeBitMapDescriptor(kFinished),

  // Has it been destroyed.
  destroyed: makeBitMapDescriptor(kDestroyed),

  // Should we decode strings into buffers before passing to _write?
  // this is here so that some node-core streams can optimize string
  // handling at a lower level.
  decodeStrings: makeBitMapDescriptor(kDecodeStrings),

  // A flag to see when we're in the middle of a write.
  writing: makeBitMapDescriptor(kWriting),

  // A flag to be able to tell if the onwrite cb is called immediately,
  // or on a later tick.  We set this to true at first, because any
  // actions that shouldn't happen until "later" should generally also
  // not happen before the first write call.
  sync: makeBitMapDescriptor(kSync),

  // A flag to know if we're processing previously buffered items, which
  // may call the _write() callback in the same tick, so that we don't
  // end up in an overlapped onwrite situation.
  bufferProcessing: makeBitMapDescriptor(kBufferProcessing),

  // Stream is still being constructed and cannot be
  // destroyed until construction finished or failed.
  // Async construction is opt in, therefore we start as
  // constructed.
  constructed: makeBitMapDescriptor(kConstructed),

  // Emit prefinish if the only thing we're waiting for is _write cbs
  // This is relevant for synchronous Transform streams.
  prefinished: makeBitMapDescriptor(kPrefinished),

  // True if the error was already emitted and should not be thrown again.
  errorEmitted: makeBitMapDescriptor(kErrorEmitted),

  // Should close be emitted on destroy. Defaults to true.
  emitClose: makeBitMapDescriptor(kEmitClose),

  // Should .destroy() be called after 'finish' (and potentially 'end').
  autoDestroy: makeBitMapDescriptor(kAutoDestroy),

  // Indicates whether the stream has finished destroying.
  closed: makeBitMapDescriptor(kClosed),

  // True if close has been emitted or would have been emitted
  // depending on emitClose.
  closeEmitted: makeBitMapDescriptor(kCloseEmitted),

  allBuffers: makeBitMapDescriptor(kAllBuffers),
  allNoop: makeBitMapDescriptor(kAllNoop),

  // Indicates whether the stream has errored. When true all write() calls
  // should return false. This is needed since when autoDestroy
  // is disabled we need a way to tell whether the stream has failed.
  // This is/should be a cold path.
  errored: {
    __proto__: null,
    enumerable: false,
    get() {
      return (this[kState] & kErrored) !== 0 ? this[kErroredValue] : null;
    },
    set(value) {
      if (value) {
        this[kErroredValue] = value;
        this[kState] |= kErrored;
      } else {
        this[kState] &= ~kErrored;
      }
    },
  },

  writable: {
    __proto__: null,
    enumerable: false,
    get() {
      return (this[kState] & kHasWritable) !== 0
        ? (this[kState] & kWritable) !== 0
        : undefined;
    },
    set(value) {
      if (value == null) {
        this[kState] &= ~(kHasWritable | kWritable);
      } else if (value) {
        this[kState] |= kHasWritable | kWritable;
      } else {
        this[kState] |= kHasWritable;
        this[kState] &= ~kWritable;
      }
    },
  },

  defaultEncoding: {
    __proto__: null,
    enumerable: false,
    get() {
      return (this[kState] & kDefaultUTF8Encoding) !== 0
        ? 'utf8'
        : this[kDefaultEncodingValue];
    },
    set(value) {
      if (value === 'utf8' || value === 'utf-8') {
        this[kState] |= kDefaultUTF8Encoding;
      } else {
        this[kState] &= ~kDefaultUTF8Encoding;
        this[kDefaultEncodingValue] = value;
      }
    },
  },

  // The callback that the user supplies to write(chunk, encoding, cb).
  writecb: {
    __proto__: null,
    enumerable: false,
    get() {
      return (this[kState] & kWriteCb) !== 0 ? this[kWriteCbValue] : nop;
    },
    set(value) {
      this[kWriteCbValue] = value;
      if (value) {
        this[kState] |= kWriteCb;
      } else {
        this[kState] &= ~kWriteCb;
      }
    },
  },

  // Storage for data passed to the afterWrite() callback in case of
  // synchronous _write() completion.
  afterWriteTickInfo: {
    __proto__: null,
    enumerable: false,
    get() {
      return (this[kState] & kAfterWriteTickInfo) !== 0
        ? this[kAfterWriteTickInfoValue]
        : null;
    },
    set(value) {
      this[kAfterWriteTickInfoValue] = value;
      if (value) {
        this[kState] |= kAfterWriteTickInfo;
      } else {
        this[kState] &= ~kAfterWriteTickInfo;
      }
    },
  },

  buffered: {
    __proto__: null,
    enumerable: false,
    get() {
      return (this[kState] & kBuffered) !== 0 ? this[kBufferedValue] : [];
    },
    set(value) {
      this[kBufferedValue] = value;
      if (value) {
        this[kState] |= kBuffered;
      } else {
        this[kState] &= ~kBuffered;
      }
    },
  },
});

// ======================================================================================
// WritableState

export function WritableState(options, stream, isDuplex) {
  // Bit map field to store WritableState more efficiently with 1 bit per field
  // instead of a V8 slot per field.
  this[kState] = kSync | kConstructed | kEmitClose | kAutoDestroy;

  if (options?.objectMode) this[kState] |= kObjectMode;

  if (isDuplex && options?.writableObjectMode) this[kState] |= kObjectMode;

  // The point at which write() starts returning false
  // Note: 0 is a valid value, means that we always return false if
  // the entire buffer is not flushed immediately on write().
  this.highWaterMark = options
    ? getHighWaterMark(this, options, 'writableHighWaterMark', isDuplex)
    : getDefaultHighWaterMark(false);

  if (!options || options.decodeStrings !== false)
    this[kState] |= kDecodeStrings;

  // Should close be emitted on destroy. Defaults to true.
  if (options && options.emitClose === false) this[kState] &= ~kEmitClose;

  // Should .destroy() be called after 'end' (and potentially 'finish').
  if (options && options.autoDestroy === false) this[kState] &= ~kAutoDestroy;

  // Crypto is kind of old and crusty.  Historically, its default string
  // encoding is 'binary' so we have to make this configurable.
  // Everything else in the universe uses 'utf8', though.
  const defaultEncoding = options ? options.defaultEncoding : null;
  if (
    defaultEncoding == null ||
    defaultEncoding === 'utf8' ||
    defaultEncoding === 'utf-8'
  ) {
    this[kState] |= kDefaultUTF8Encoding;
  } else if (Buffer.isEncoding(defaultEncoding)) {
    this[kState] &= ~kDefaultUTF8Encoding;
    this[kDefaultEncodingValue] = defaultEncoding;
  } else if (streamsNodejsV24Compat) {
    // This is a semver-major change. Ref: https://github.com/nodejs/node/pull/46322
    throw new ERR_UNKNOWN_ENCODING(defaultEncoding);
  } else {
    this[kDefaultEncodingValue] = defaultEncoding;
  }

  // Not an actual buffer we keep track of, but a measurement
  // of how much we're waiting to get pushed to some underlying
  // socket or file.
  this.length = 0;

  // When true all writes will be buffered until .uncork() call.
  this.corked = 0;

  // The callback that's passed to _write(chunk, cb).
  this.onwrite = onwrite.bind(undefined, stream);

  // The amount that is being written when _write is called.
  this.writelen = 0;

  resetBuffer(this);

  // Number of pending user-supplied write callbacks
  // this must be 0 before 'finish' can be emitted.
  this.pendingcb = 0;
}

function resetBuffer(state) {
  state[kBufferedValue] = null;
  state.bufferedIndex = 0;
  state[kState] |= kAllBuffers | kAllNoop;
  state[kState] &= ~kBuffered;
}

WritableState.prototype.getBuffer = function getBuffer() {
  return (this[kState] & kBuffered) === 0
    ? []
    : this.buffered.slice(this.bufferedIndex);
};

Object.defineProperty(WritableState.prototype, 'bufferedRequestCount', {
  __proto__: null,
  get() {
    return (this[kState] & kBuffered) === 0
      ? 0
      : this[kBufferedValue].length - this.bufferedIndex;
  },
});

WritableState.prototype[kOnConstructed] = function onConstructed(stream) {
  if ((this[kState] & kWriting) === 0) {
    clearBuffer(stream, this);
  }

  if ((this[kState] & kEnding) !== 0) {
    finishMaybe(stream, this);
  }
};

// ======================================================================================
// Writable

Writable.WritableState = WritableState;

Object.setPrototypeOf(Writable.prototype, Stream.prototype);
Object.setPrototypeOf(Writable, Stream);

export function Writable(options) {
  if (!(this instanceof Writable)) return new Writable(options);

  this._events ??= {
    close: undefined,
    error: undefined,
    prefinish: undefined,
    finish: undefined,
    drain: undefined,
    // Skip uncommon events...
    // [destroyImpl.kConstruct]: undefined,
    // [destroyImpl.kDestroy]: undefined,
  };

  this._writableState = new WritableState(options, this, false);

  if (options) {
    if (typeof options.write === 'function') this._write = options.write;

    if (typeof options.writev === 'function') this._writev = options.writev;

    if (typeof options.destroy === 'function') this._destroy = options.destroy;

    if (typeof options.final === 'function') this._final = options.final;

    if (typeof options.construct === 'function')
      this._construct = options.construct;

    if (options.signal) addAbortSignal(options.signal, this);
  }

  Stream.call(this, options);

  if (this._construct != null) {
    destroyImpl.construct(this, () => {
      this._writableState[kOnConstructed](this);
    });
  }
}

Object.defineProperty(Writable, Symbol.hasInstance, {
  __proto__: null,
  value: function (object) {
    if (Function.prototype[Symbol.hasInstance].call(this, object)) return true;
    if (this !== Writable) return false;

    return object && object._writableState instanceof WritableState;
  },
});

// Otherwise people can pipe Writable streams, which is just wrong.
Writable.prototype.pipe = function () {
  destroyImpl.errorOrDestroy(this, new ERR_STREAM_CANNOT_PIPE());
};

function _write(stream, chunk, encoding, cb) {
  const state = stream._writableState;

  if (cb == null || typeof cb !== 'function') {
    cb = nop;
  }

  if (chunk === null) {
    throw new ERR_STREAM_NULL_VALUES();
  }

  if ((state[kState] & kObjectMode) === 0) {
    if (!encoding) {
      encoding =
        (state[kState] & kDefaultUTF8Encoding) !== 0
          ? 'utf8'
          : state.defaultEncoding;
    } else if (encoding !== 'buffer' && !Buffer.isEncoding(encoding)) {
      throw new ERR_UNKNOWN_ENCODING(encoding);
    }

    if (typeof chunk === 'string') {
      if ((state[kState] & kDecodeStrings) !== 0) {
        chunk = Buffer.from(chunk, encoding);
        encoding = 'buffer';
      }
    } else if (chunk instanceof Buffer) {
      encoding = 'buffer';
    } else if (Stream._isArrayBufferView(chunk)) {
      chunk = Stream._uint8ArrayToBuffer(chunk);
      encoding = 'buffer';
    } else {
      throw new ERR_INVALID_ARG_TYPE(
        'chunk',
        ['string', 'Buffer', 'TypedArray', 'DataView'],
        chunk
      );
    }
  }

  let err;
  if ((state[kState] & kEnding) !== 0) {
    err = new ERR_STREAM_WRITE_AFTER_END();
  } else if ((state[kState] & kDestroyed) !== 0) {
    err = new ERR_STREAM_DESTROYED('write');
  }

  if (err) {
    nextTick(cb, err);
    destroyImpl.errorOrDestroy(stream, err, true);
    return err;
  }

  state.pendingcb++;
  return writeOrBuffer(stream, state, chunk, encoding, cb);
}

Writable.prototype.write = function (chunk, encoding, cb) {
  if (encoding != null && typeof encoding === 'function') {
    cb = encoding;
    encoding = null;
  }

  return _write(this, chunk, encoding, cb) === true;
};

Writable.prototype.cork = function () {
  const state = this._writableState;

  state[kState] |= kCorked;
  state.corked++;
};

Writable.prototype.uncork = function () {
  const state = this._writableState;

  if (state.corked) {
    state.corked--;

    if (!state.corked) {
      state[kState] &= ~kCorked;
    }

    if ((state[kState] & kWriting) === 0) clearBuffer(this, state);
  }
};

Writable.prototype.setDefaultEncoding = function setDefaultEncoding(encoding) {
  // node::ParseEncoding() requires lower case.
  if (typeof encoding === 'string') encoding = encoding.toLowerCase();
  if (!Buffer.isEncoding(encoding)) throw new ERR_UNKNOWN_ENCODING(encoding);
  this._writableState.defaultEncoding = encoding;
  return this;
};

// If we're already writing something, then just put this
// in the queue, and wait our turn.  Otherwise, call _write
// If we return false, then we need a drain event, so set that flag.
function writeOrBuffer(stream, state, chunk, encoding, callback) {
  const len = (state[kState] & kObjectMode) !== 0 ? 1 : chunk.length;

  state.length += len;

  // This is a semver-major change. Ref: https://github.com/nodejs/node/commit/557044af407376aff28a0a0800f3053bb58e9239
  //
  // The timing of backpressure (ret) calculation relative to _write() execution is critical.
  // When _write() completes synchronously and modifies state.length, calculating ret before
  // the write uses stale buffer state, leading to incorrect needDrain signaling and stream hangs.
  // v24+ calculates ret after _write() to ensure backpressure reflects post-write buffer state.
  let ret;
  if (!streamsNodejsV24Compat) {
    ret = state.length < state.highWaterMark || state.length === 0;
    // We must ensure that previous needDrain will not be reset to false.
    if (!ret) {
      state[kState] |= kNeedDrain;
    }
  }

  if (
    (state[kState] & (kWriting | kErrored | kCorked | kConstructed)) !==
    kConstructed
  ) {
    if ((state[kState] & kBuffered) === 0) {
      state[kState] |= kBuffered;
      state[kBufferedValue] = [];
    }

    state[kBufferedValue].push({ chunk, encoding, callback });
    if ((state[kState] & kAllBuffers) !== 0 && encoding !== 'buffer') {
      state[kState] &= ~kAllBuffers;
    }
    if ((state[kState] & kAllNoop) !== 0 && callback !== nop) {
      state[kState] &= ~kAllNoop;
    }
  } else {
    state.writelen = len;
    if (callback !== nop) {
      state.writecb = callback;
    }
    state[kState] |= kWriting | kSync | kExpectWriteCb;
    stream._write(chunk, encoding, state.onwrite);
    state[kState] &= ~kSync;
  }

  // This is a semver-major change. Ref: https://github.com/nodejs/node/commit/557044af407376aff28a0a0800f3053bb58e9239
  // For v24+, calculate ret after _write() to observe post-write buffer state.
  if (streamsNodejsV24Compat) {
    ret = state.length < state.highWaterMark || state.length === 0;
    // We must ensure that previous needDrain will not be reset to false.
    if (!ret) {
      state[kState] |= kNeedDrain;
    }
  }

  // Return false if errored or destroyed in order to break
  // any synchronous while(stream.write(data)) loops.
  return ret && (state[kState] & (kDestroyed | kErrored)) === 0;
}

function doWrite(stream, state, writev, len, chunk, encoding, cb) {
  state.writelen = len;
  if (cb !== nop) {
    state.writecb = cb;
  }
  state[kState] |= kWriting | kSync | kExpectWriteCb;
  if ((state[kState] & kDestroyed) !== 0)
    state.onwrite(new ERR_STREAM_DESTROYED('write'));
  else if (writev) stream._writev(chunk, state.onwrite);
  else stream._write(chunk, encoding, state.onwrite);
  state[kState] &= ~kSync;
}

function onwriteError(stream, state, er, cb) {
  --state.pendingcb;
  cb(er);
  // Ensure callbacks are invoked even when autoDestroy is
  // not enabled. Passing `er` here doesn't make sense since
  // it's related to one specific write, not to the buffered
  // writes.
  errorBuffer(state);
  // This can emit error, but error must always follow cb.
  destroyImpl.errorOrDestroy(stream, er);
}

function onwrite(stream, er) {
  const state = stream._writableState;

  if ((state[kState] & kExpectWriteCb) === 0) {
    destroyImpl.errorOrDestroy(stream, new ERR_MULTIPLE_CALLBACK());
    return;
  }

  const sync = (state[kState] & kSync) !== 0;
  const cb = (state[kState] & kWriteCb) !== 0 ? state[kWriteCbValue] : nop;

  state.writecb = null;
  state[kState] &= ~(kWriting | kExpectWriteCb);
  state.length -= state.writelen;
  state.writelen = 0;

  if (er) {
    // Avoid V8 leak, https://github.com/nodejs/node/pull/34103#issuecomment-652002364
    er.stack; // eslint-disable-line @typescript-eslint/no-unused-expressions

    if ((state[kState] & kErrored) === 0) {
      state[kErroredValue] = er;
      state[kState] |= kErrored;
    }

    // In case of duplex streams we need to notify the readable side of the
    // error.
    if (stream._readableState && !stream._readableState.errored) {
      stream._readableState.errored = er;
    }

    if (sync) {
      nextTick(onwriteError, stream, state, er, cb);
    } else {
      onwriteError(stream, state, er, cb);
    }
  } else {
    if ((state[kState] & kBuffered) !== 0) {
      clearBuffer(stream, state);
    }

    if (sync) {
      const needDrain =
        (state[kState] & kNeedDrain) !== 0 && state.length === 0;
      const needTick =
        needDrain || state[kState] & (kDestroyed !== 0) || cb !== nop;

      // It is a common case that the callback passed to .write() is always
      // the same. In that case, we do not schedule a new nextTick(), but
      // rather just increase a counter, to improve performance and avoid
      // memory allocations.
      if (cb === nop) {
        if ((state[kState] & kAfterWritePending) === 0 && needTick) {
          nextTick(afterWrite, stream, state, 1, cb);
          state[kState] |= kAfterWritePending;
        } else {
          state.pendingcb--;
          if ((state[kState] & kEnding) !== 0) {
            finishMaybe(stream, state, true);
          }
        }
      } else if (
        (state[kState] & kAfterWriteTickInfo) !== 0 &&
        state[kAfterWriteTickInfoValue].cb === cb
      ) {
        state[kAfterWriteTickInfoValue].count++;
      } else if (needTick) {
        state[kAfterWriteTickInfoValue] = { count: 1, cb, stream, state };
        nextTick(afterWriteTick, state[kAfterWriteTickInfoValue]);
        state[kState] |= kAfterWritePending | kAfterWriteTickInfo;
      } else {
        state.pendingcb--;
        if ((state[kState] & kEnding) !== 0) {
          finishMaybe(stream, state, true);
        }
      }
    } else {
      afterWrite(stream, state, 1, cb);
    }
  }
}

function afterWriteTick({ stream, state, count, cb }) {
  state[kState] &= ~kAfterWriteTickInfo;
  state[kAfterWriteTickInfoValue] = null;
  return afterWrite(stream, state, count, cb);
}

function afterWrite(stream, state, count, cb) {
  state[kState] &= ~kAfterWritePending;

  const needDrain =
    (state[kState] & (kEnding | kNeedDrain | kDestroyed)) === kNeedDrain &&
    state.length === 0;
  if (needDrain) {
    state[kState] &= ~kNeedDrain;
    stream.emit('drain');
  }

  // This is a semver-major change. Ref: https://github.com/nodejs/node/pull/44312/files
  const callbackValue = streamsNodejsV24Compat ? null : undefined;
  while (count-- > 0) {
    state.pendingcb--;
    cb(callbackValue);
  }

  if ((state[kState] & kDestroyed) !== 0) {
    errorBuffer(state);
  }

  if ((state[kState] & kEnding) !== 0) {
    finishMaybe(stream, state, true);
  }
}

// If there's something in the buffer waiting, then invoke callbacks.
function errorBuffer(state) {
  if ((state[kState] & kWriting) !== 0) {
    return;
  }

  if ((state[kState] & kBuffered) !== 0) {
    for (let n = state.bufferedIndex; n < state.buffered.length; ++n) {
      const { chunk, callback } = state[kBufferedValue][n];
      const len = (state[kState] & kObjectMode) !== 0 ? 1 : chunk.length;
      state.length -= len;
      callback(state.errored ?? new ERR_STREAM_DESTROYED('write'));
    }
  }

  callFinishedCallbacks(
    state,
    state.errored ?? new ERR_STREAM_DESTROYED('end')
  );

  resetBuffer(state);
}

// If there's something in the buffer waiting, then process it.
function clearBuffer(stream, state) {
  if (
    (state[kState] &
      (kDestroyed | kBufferProcessing | kCorked | kBuffered | kConstructed)) !==
    (kBuffered | kConstructed)
  ) {
    return;
  }

  const objectMode = (state[kState] & kObjectMode) !== 0;
  const { [kBufferedValue]: buffered, bufferedIndex } = state;
  const bufferedLength = buffered.length - bufferedIndex;

  if (!bufferedLength) {
    return;
  }

  let i = bufferedIndex;

  state[kState] |= kBufferProcessing;
  if (bufferedLength > 1 && stream._writev) {
    state.pendingcb -= bufferedLength - 1;

    const callback =
      (state[kState] & kAllNoop) !== 0
        ? nop
        : (err) => {
            for (let n = i; n < buffered.length; ++n) {
              buffered[n].callback(err);
            }
          };
    // Make a copy of `buffered` if it's going to be used by `callback` above,
    // since `doWrite` will mutate the array.
    const chunks =
      (state[kState] & kAllNoop) !== 0 && i === 0
        ? buffered
        : buffered.slice(i);
    chunks.allBuffers = (state[kState] & kAllBuffers) !== 0;

    doWrite(stream, state, true, state.length, chunks, '', callback);

    resetBuffer(state);
  } else {
    do {
      const { chunk, encoding, callback } = buffered[i];
      buffered[i++] = null;
      const len = objectMode ? 1 : chunk.length;
      doWrite(stream, state, false, len, chunk, encoding, callback);
    } while (i < buffered.length && (state[kState] & kWriting) === 0);

    if (i === buffered.length) {
      resetBuffer(state);
    } else if (i > 256) {
      buffered.splice(0, i);
      state.bufferedIndex = 0;
    } else {
      state.bufferedIndex = i;
    }
  }
  state[kState] &= ~kBufferProcessing;
}

Writable.prototype._write = function (chunk, encoding, cb) {
  if (this._writev) {
    this._writev([{ chunk, encoding }], cb);
  } else {
    throw new ERR_METHOD_NOT_IMPLEMENTED('_write()');
  }
};

Writable.prototype._writev = null;

Writable.prototype.end = function (chunk, encoding, cb) {
  const state = this._writableState;

  if (typeof chunk === 'function') {
    cb = chunk;
    chunk = null;
    encoding = null;
  } else if (typeof encoding === 'function') {
    cb = encoding;
    encoding = null;
  }

  let err;

  if (chunk != null) {
    const ret = _write(this, chunk, encoding);
    if (ret instanceof Error) {
      err = ret;
    }
  }

  // .end() fully uncorks.
  if ((state[kState] & kCorked) !== 0) {
    state.corked = 1;
    this.uncork();
  }

  if (err) {
    // Do nothing...
  } else if ((state[kState] & (kEnding | kErrored)) === 0) {
    // This is forgiving in terms of unnecessary calls to end() and can hide
    // logic errors. However, usually such errors are harmless and causing a
    // hard error can be disproportionately destructive. It is not always
    // trivial for the user to determine whether end() needs to be called
    // or not.

    state[kState] |= kEnding;
    finishMaybe(this, state, true);
    state[kState] |= kEnded;
  } else if ((state[kState] & kFinished) !== 0) {
    err = new ERR_STREAM_ALREADY_FINISHED('end');
  } else if ((state[kState] & kDestroyed) !== 0) {
    err = new ERR_STREAM_DESTROYED('end');
  }

  if (typeof cb === 'function') {
    // This is a semver-major change. Ref: https://github.com/nodejs/node/pull/44312
    if (streamsNodejsV24Compat) {
      if (err) {
        nextTick(cb, err);
      } else if ((state[kState] & kErrored) !== 0) {
        nextTick(cb, state[kErroredValue]);
      } else if ((state[kState] & kFinished) !== 0) {
        nextTick(cb, null);
      } else {
        state[kState] |= kOnFinished;
        state[kOnFinishedValue] ??= [];
        state[kOnFinishedValue].push(cb);
      }
    } else {
      if (err || (state[kState] & kFinished) !== 0) {
        nextTick(cb, err);
      } else if ((state[kState] & kErrored) !== 0) {
        nextTick(cb, state[kErroredValue]);
      } else {
        state[kState] |= kOnFinished;
        state[kOnFinishedValue] ??= [];
        state[kOnFinishedValue].push(cb);
      }
    }
  }

  return this;
};

function needFinish(state) {
  return (
    // State is ended && constructed but not destroyed, finished, writing, errorEmitted or closedEmitted
    (state[kState] &
      (kEnding |
        kDestroyed |
        kConstructed |
        kFinished |
        kWriting |
        kErrorEmitted |
        kCloseEmitted |
        kErrored |
        kBuffered)) ===
      (kEnding | kConstructed) && state.length === 0
  );
}
function onFinish(stream, state, err) {
  if ((state[kState] & kPrefinished) !== 0) {
    destroyImpl.errorOrDestroy(stream, err ?? new ERR_MULTIPLE_CALLBACK());
    return;
  }
  state.pendingcb--;
  if (err) {
    callFinishedCallbacks(state, err);
    destroyImpl.errorOrDestroy(stream, err, (state[kState] & kSync) !== 0);
  } else if (needFinish(state)) {
    state[kState] |= kPrefinished;
    stream.emit('prefinish');
    // Backwards compat. Don't check state.sync here.
    // Some streams assume 'finish' will be emitted
    // asynchronously relative to _final callback.
    state.pendingcb++;
    nextTick(finish, stream, state);
  }
}

function prefinish(stream, state) {
  if ((state[kState] & (kPrefinished | kFinalCalled)) !== 0) {
    return;
  }

  if (
    typeof stream._final === 'function' &&
    (state[kState] & kDestroyed) === 0
  ) {
    state[kState] |= kFinalCalled | kSync;
    state.pendingcb++;

    try {
      stream._final((err) => onFinish(stream, state, err));
    } catch (err) {
      onFinish(stream, state, err);
    }

    state[kState] &= ~kSync;
  } else {
    state[kState] |= kFinalCalled | kPrefinished;
    stream.emit('prefinish');
  }
}

function finishMaybe(stream, state, sync) {
  if (needFinish(state)) {
    prefinish(stream, state);
    if (state.pendingcb === 0) {
      if (sync) {
        state.pendingcb++;
        nextTick(
          (stream, state) => {
            if (needFinish(state)) {
              finish(stream, state);
            } else {
              state.pendingcb--;
            }
          },
          stream,
          state
        );
      } else if (needFinish(state)) {
        state.pendingcb++;
        finish(stream, state);
      }
    }
  }
}

function finish(stream, state) {
  state.pendingcb--;
  state[kState] |= kFinished;

  callFinishedCallbacks(state, null);

  stream.emit('finish');

  if ((state[kState] & kAutoDestroy) !== 0) {
    // In case of duplex streams we need a way to detect
    // if the readable side is ready for autoDestroy as well.
    const rState = stream._readableState;
    const autoDestroy =
      !rState ||
      (rState.autoDestroy &&
        // We don't expect the readable to ever 'end'
        // if readable is explicitly set to false.
        (rState.endEmitted || rState.readable === false));
    if (autoDestroy) {
      stream.destroy();
    }
  }
}

function callFinishedCallbacks(state, err) {
  if ((state[kState] & kOnFinished) === 0) {
    return;
  }

  const onfinishCallbacks = state[kOnFinishedValue];
  // This is a semver-major change. Ref: https://github.com/nodejs/node/pull/44312
  state[kOnFinishedValue] = streamsNodejsV24Compat ? null : undefined;
  state[kState] &= ~kOnFinished;
  for (let i = 0; i < onfinishCallbacks.length; i++) {
    onfinishCallbacks[i](err);
  }
}

Object.defineProperties(Writable.prototype, {
  closed: {
    __proto__: null,
    get() {
      return this._writableState
        ? (this._writableState[kState] & kClosed) !== 0
        : false;
    },
  },

  destroyed: {
    __proto__: null,
    get() {
      return this._writableState
        ? (this._writableState[kState] & kDestroyed) !== 0
        : false;
    },
    set(value) {
      // Backward compatibility, the user is explicitly managing destroyed.
      if (!this._writableState) return;

      if (value) this._writableState[kState] |= kDestroyed;
      else this._writableState[kState] &= ~kDestroyed;
    },
  },

  writable: {
    __proto__: null,
    get() {
      const w = this._writableState;
      // w.writable === false means that this is part of a Duplex stream
      // where the writable side was disabled upon construction.
      // Compat. The user might manually disable writable side through
      // deprecated setter.
      return (
        !!w &&
        w.writable !== false &&
        (w[kState] & (kEnding | kEnded | kDestroyed | kErrored)) === 0
      );
    },
    set(val) {
      // Backwards compatible.
      if (this._writableState) {
        this._writableState.writable = !!val;
      }
    },
  },

  writableFinished: {
    __proto__: null,
    get() {
      const state = this._writableState;
      return state ? (state[kState] & kFinished) !== 0 : false;
    },
  },

  writableObjectMode: {
    __proto__: null,
    get() {
      const state = this._writableState;
      return state ? (state[kState] & kObjectMode) !== 0 : false;
    },
  },

  writableBuffer: {
    __proto__: null,
    get() {
      const state = this._writableState;
      return state && state.getBuffer();
    },
  },

  writableEnded: {
    __proto__: null,
    get() {
      const state = this._writableState;
      return state ? (state[kState] & kEnding) !== 0 : false;
    },
  },

  writableNeedDrain: {
    __proto__: null,
    get() {
      const state = this._writableState;
      return state
        ? (state[kState] & (kDestroyed | kEnding | kNeedDrain)) === kNeedDrain
        : false;
    },
  },

  writableHighWaterMark: {
    __proto__: null,
    get() {
      const state = this._writableState;
      return state?.highWaterMark;
    },
  },

  writableCorked: {
    __proto__: null,
    get() {
      const state = this._writableState;
      return state ? state.corked : 0;
    },
  },

  writableLength: {
    __proto__: null,
    get() {
      const state = this._writableState;
      return state?.length;
    },
  },

  errored: {
    __proto__: null,
    enumerable: false,
    get() {
      const state = this._writableState;
      return state ? state.errored : null;
    },
  },

  writableAborted: {
    __proto__: null,
    get: function () {
      const state = this._writableState;
      return (
        (state[kState] & (kHasWritable | kWritable)) !== kHasWritable &&
        (state[kState] & (kDestroyed | kErrored)) !== 0 &&
        (state[kState] & kFinished) === 0
      );
    },
  },
});

const destroy = destroyImpl.destroy;
Writable.prototype.destroy = function (err, cb) {
  const state = this._writableState;

  // Invoke pending callbacks.
  if (
    (state[kState] & (kBuffered | kOnFinished)) !== 0 &&
    (state[kState] & kDestroyed) === 0
  ) {
    nextTick(errorBuffer, state);
  }

  destroy.call(this, err, cb);
  return this;
};

Writable.prototype._undestroy = destroyImpl.undestroy;
Writable.prototype._destroy = function (err, cb) {
  cb(err);
};

Writable.prototype[EventEmitter.captureRejectionSymbol] = function (err) {
  this.destroy(err);
};

export function fromWeb(writableStream, options) {
  return newStreamWritableFromWritableStream(writableStream, options);
}

export function toWeb(streamWritable) {
  return newWritableStreamFromStreamWritable(streamWritable);
}

Writable.fromWeb = fromWeb;
Writable.toWeb = toWeb;

Writable.prototype[Symbol.asyncDispose] = async function () {
  let error;
  if (!this.destroyed) {
    error = this.writableFinished ? null : new AbortError();
    this.destroy(error);
  }
  await new Promise((resolve, reject) =>
    eos(this, (err) =>
      err && err.name !== 'AbortError' ? reject(err) : resolve(null)
    )
  );
};

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
    const writable = new globalThis.WritableStream();
    writable.close();
    return writable;
  }

  const highWaterMark = streamWritable.writableHighWaterMark;
  const strategy = streamWritable.writableObjectMode
    ? new globalThis.CountQueuingStrategy({ highWaterMark })
    : { highWaterMark };

  let controller;
  let backpressurePromise;
  let closed;

  function onDrain() {
    if (backpressurePromise !== undefined) backpressurePromise.resolve();
  }

  const cleanup = finished(streamWritable, (error) => {
    error = handleKnownInternalErrors(error);

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

  return new globalThis.WritableStream(
    {
      start(c) {
        controller = c;
      },

      write(chunk) {
        if (streamWritable.writableNeedDrain || !streamWritable.write(chunk)) {
          backpressurePromise = Promise.withResolvers();
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
          closed = Promise.withResolvers();
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
  if (!isWritableStream(writableStream)) {
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
          nextTick(() => destroy(writable, error));
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
          nextTick(() => {
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
          nextTick(() => destroy(writable, error));
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
