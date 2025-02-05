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

export const kDestroyed = Symbol('kDestroyed');
export const kIsErrored = Symbol('kIsErrored');
export const kIsReadable = Symbol('kIsReadable');
export const kIsDisturbed = Symbol('kIsDisturbed');
export const kPaused = Symbol('kPaused');
export const kOnFinished = Symbol('kOnFinished');
export const kDestroy = Symbol('kDestroy');
export const kConstruct = Symbol('kConstruct');

import {
  AbortError,
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_STREAM_PREMATURE_CLOSE,
  ERR_MULTIPLE_CALLBACK,
  aggregateTwoErrors,
} from 'node-internal:internal_errors';

import * as process from 'node-internal:process';

import { Buffer } from 'node-internal:internal_buffer';

import {
  validateAbortSignal,
  validateFunction,
  validateObject,
} from 'node-internal:validators';

export function isReadableNodeStream(obj, strict = false) {
  let _obj$_readableState;
  return !!(
    (
      obj &&
      typeof obj.pipe === 'function' &&
      typeof obj.on === 'function' &&
      (!strict ||
        (typeof obj.pause === 'function' &&
          typeof obj.resume === 'function')) &&
      (!obj._writableState ||
        ((_obj$_readableState = obj._readableState) === null ||
        _obj$_readableState === undefined
          ? undefined
          : _obj$_readableState.readable) !== false) &&
      // Duplex
      (!obj._writableState || obj._readableState)
    ) // Writable has .pipe.
  );
}

export function isWritableNodeStream(obj) {
  let _obj$_writableState;
  return !!(
    (
      obj &&
      typeof obj.write === 'function' &&
      typeof obj.on === 'function' &&
      (!obj._readableState ||
        ((_obj$_writableState = obj._writableState) === null ||
        _obj$_writableState === undefined
          ? undefined
          : _obj$_writableState.writable) !== false)
    ) // Duplex
  );
}

export function isDuplexNodeStream(obj) {
  return !!(
    obj &&
    typeof obj.pipe === 'function' &&
    obj._readableState &&
    typeof obj.on === 'function' &&
    typeof obj.write === 'function'
  );
}

export function isNodeStream(obj) {
  return (
    obj &&
    (obj._readableState ||
      obj._writableState ||
      (typeof obj.write === 'function' && typeof obj.on === 'function') ||
      (typeof obj.pipe === 'function' && typeof obj.on === 'function'))
  );
}

export function isIterable(obj, isAsync = false) {
  if (obj == null) return false;
  if (isAsync === true) return typeof obj[Symbol.asyncIterator] === 'function';
  if (isAsync === false) return typeof obj[Symbol.iterator] === 'function';
  return (
    typeof obj[Symbol.asyncIterator] === 'function' ||
    typeof obj[Symbol.iterator] === 'function'
  );
}

export function isDestroyed(stream) {
  if (!isNodeStream(stream)) return null;
  const wState = stream._writableState;
  const rState = stream._readableState;
  const state = wState || rState;
  return !!(
    stream.destroyed ||
    stream[kDestroyed] ||
    (state !== null && state !== undefined && state.destroyed)
  );
}

export function isWritableEnded(stream) {
  if (!isWritableNodeStream(stream)) return null;
  if (stream.writableEnded === true) return true;
  const wState = stream._writableState;
  if (wState !== null && wState !== undefined && wState.errored) return false;
  if (
    typeof (wState === null || wState === undefined
      ? undefined
      : wState.ended) !== 'boolean'
  )
    return null;
  return wState.ended;
}

export function isWritableFinished(stream, strict = false) {
  if (!isWritableNodeStream(stream)) return null;
  if (stream.writableFinished === true) return true;
  const wState = stream._writableState;
  if (wState !== null && wState !== undefined && wState.errored) return false;
  if (
    typeof (wState === null || wState === undefined
      ? undefined
      : wState.finished) !== 'boolean'
  )
    return null;
  return !!(
    wState.finished ||
    (strict === false && wState.ended === true && wState.length === 0)
  );
}

export function isReadableEnded(stream) {
  if (!isReadableNodeStream(stream)) return null;
  if (stream.readableEnded === true) return true;
  const rState = stream._readableState;
  if (!rState || rState.errored) return false;
  if (
    typeof (rState === null || rState === undefined
      ? undefined
      : rState.ended) !== 'boolean'
  )
    return null;
  return rState.ended;
}

export function isReadableFinished(stream, strict = false) {
  if (!isReadableNodeStream(stream)) return null;
  const rState = stream._readableState;
  if (rState !== null && rState !== undefined && rState.errored) return false;
  if (
    typeof (rState === null || rState === undefined
      ? undefined
      : rState.endEmitted) !== 'boolean'
  )
    return null;
  return !!(
    rState.endEmitted ||
    (strict === false && rState.ended === true && rState.length === 0)
  );
}

export function isReadable(stream) {
  if (stream && stream[kIsReadable] != null) return stream[kIsReadable];
  if (
    typeof (stream === null || stream === undefined
      ? undefined
      : stream.readable) !== 'boolean'
  )
    return null;
  if (isDestroyed(stream)) return false;
  return (
    isReadableNodeStream(stream) &&
    stream.readable &&
    !isReadableFinished(stream)
  );
}

export function isWritable(stream) {
  if (
    typeof (stream === null || stream === undefined
      ? undefined
      : stream.writable) !== 'boolean'
  )
    return null;
  if (isDestroyed(stream)) return false;
  return (
    isWritableNodeStream(stream) && stream.writable && !isWritableEnded(stream)
  );
}

export function isFinished(stream, opts = {}) {
  if (!isNodeStream(stream)) {
    return null;
  }
  if (isDestroyed(stream)) {
    return true;
  }
  if (
    (opts === null || opts === undefined ? undefined : opts.readable) !==
      false &&
    isReadable(stream)
  ) {
    return false;
  }
  if (
    (opts === null || opts === undefined ? undefined : opts.writable) !==
      false &&
    isWritable(stream)
  ) {
    return false;
  }
  return true;
}

export function isWritableErrored(stream) {
  let _stream$_writableStat, _stream$_writableStat2;
  if (!isNodeStream(stream)) {
    return null;
  }
  if (stream.writableErrored) {
    return stream.writableErrored;
  }
  return (_stream$_writableStat =
    (_stream$_writableStat2 = stream._writableState) === null ||
    _stream$_writableStat2 === undefined
      ? undefined
      : _stream$_writableStat2.errored) !== null &&
    _stream$_writableStat !== undefined
    ? _stream$_writableStat
    : null;
}

export function isReadableErrored(stream) {
  let _stream$_readableStat, _stream$_readableStat2;
  if (!isNodeStream(stream)) {
    return null;
  }
  if (stream.readableErrored) {
    return stream.readableErrored;
  }
  return (_stream$_readableStat =
    (_stream$_readableStat2 = stream._readableState) === null ||
    _stream$_readableStat2 === undefined
      ? undefined
      : _stream$_readableStat2.errored) !== null &&
    _stream$_readableStat !== undefined
    ? _stream$_readableStat
    : null;
}

export function isClosed(stream) {
  if (!isNodeStream(stream)) {
    return null;
  }
  if (typeof stream.closed === 'boolean') {
    return stream.closed;
  }
  const wState = stream._writableState;
  const rState = stream._readableState;
  if (
    typeof (wState === null || wState === undefined
      ? undefined
      : wState.closed) === 'boolean' ||
    typeof (rState === null || rState === undefined
      ? undefined
      : rState.closed) === 'boolean'
  ) {
    return (
      (wState === null || wState === undefined ? undefined : wState.closed) ||
      (rState === null || rState === undefined ? undefined : rState.closed)
    );
  }
  if (typeof stream._closed === 'boolean' && isOutgoingMessage(stream)) {
    return stream._closed;
  }
  return null;
}

// TODO(later): We do not actually support OutgoingMessage yet. Might not ever?
// Keeping this here tho just to keep things simple.
export function isOutgoingMessage(stream) {
  return (
    typeof stream._closed === 'boolean' &&
    typeof stream._defaultKeepAlive === 'boolean' &&
    typeof stream._removedConnection === 'boolean' &&
    typeof stream._removedContLen === 'boolean'
  );
}

// TODO(later): We do not actually support Server Response yet. Might not ever?
// Keeping this here tho just to keep things simple.
export function isServerResponse(stream) {
  return typeof stream._sent100 === 'boolean' && isOutgoingMessage(stream);
}

// TODO(later): We do not actually support Server Request yet. Might not ever?
// Keeping this here tho just to keep things simple.
export function isServerRequest(stream) {
  let _stream$req;
  return (
    typeof stream._consuming === 'boolean' &&
    typeof stream._dumped === 'boolean' &&
    ((_stream$req = stream.req) === null || _stream$req === undefined
      ? undefined
      : _stream$req.upgradeOrConnect) === undefined
  );
}

export function willEmitClose(stream) {
  if (!isNodeStream(stream)) return null;
  const wState = stream._writableState;
  const rState = stream._readableState;
  const state = wState || rState;
  return (
    (!state && isServerResponse(stream)) ||
    !!(state && state.autoDestroy && state.emitClose && state.closed === false)
  );
}

export function isDisturbed(stream) {
  let _stream$kIsDisturbed;
  return !!(
    stream &&
    ((_stream$kIsDisturbed = stream[kIsDisturbed]) !== null &&
    _stream$kIsDisturbed !== undefined
      ? _stream$kIsDisturbed
      : stream.readableDidRead || stream.readableAborted)
  );
}

export function isErrored(stream) {
  var _ref,
    _ref2,
    _ref3,
    _ref4,
    _ref5,
    _stream$kIsErrored,
    _stream$_readableStat3,
    _stream$_writableStat3,
    _stream$_readableStat4,
    _stream$_writableStat4;
  return !!(
    stream &&
    ((_ref =
      (_ref2 =
        (_ref3 =
          (_ref4 =
            (_ref5 =
              (_stream$kIsErrored = stream[kIsErrored]) !== null &&
              _stream$kIsErrored !== undefined
                ? _stream$kIsErrored
                : stream.readableErrored) !== null && _ref5 !== undefined
              ? _ref5
              : stream.writableErrored) !== null && _ref4 !== undefined
            ? _ref4
            : (_stream$_readableStat3 = stream._readableState) === null ||
                _stream$_readableStat3 === undefined
              ? undefined
              : _stream$_readableStat3.errorEmitted) !== null &&
        _ref3 !== undefined
          ? _ref3
          : (_stream$_writableStat3 = stream._writableState) === null ||
              _stream$_writableStat3 === undefined
            ? undefined
            : _stream$_writableStat3.errorEmitted) !== null &&
      _ref2 !== undefined
        ? _ref2
        : (_stream$_readableStat4 = stream._readableState) === null ||
            _stream$_readableStat4 === undefined
          ? undefined
          : _stream$_readableStat4.errored) !== null && _ref !== undefined
      ? _ref
      : (_stream$_writableStat4 = stream._writableState) === null ||
          _stream$_writableStat4 === undefined
        ? undefined
        : _stream$_writableStat4.errored)
  );
}

export const nop = () => {};

export function once(callback) {
  let called = false;
  return function (...args) {
    if (called) {
      return;
    }
    called = true;
    callback.apply(this, args);
  };
}

// ======================================================================================
// highWaterMark handling

export function highWaterMarkFrom(options, isDuplex, duplexKey) {
  return options.highWaterMark != null
    ? options.highWaterMark
    : isDuplex
      ? options[duplexKey]
      : null;
}

export function getDefaultHighWaterMark(objectMode = false) {
  return objectMode ? 16 : 16 * 1024;
}

export function setDefaultHighWaterMark() {
  throw new Error(
    'Setting the default high water mark is currently not implemented'
  );
}

export function getHighWaterMark(state, options, duplexKey, isDuplex) {
  const hwm = highWaterMarkFrom(options, isDuplex, duplexKey);
  if (hwm != null) {
    if (!Number.isInteger(hwm) || hwm < 0) {
      const name = isDuplex ? `options.${duplexKey}` : 'options.highWaterMark';
      throw new ERR_INVALID_ARG_VALUE(name, hwm, name);
    }
    return Math.floor(hwm);
  }

  // Default value
  return getDefaultHighWaterMark(state.objectMode);
}

// ======================================================================================
// addAbortSignal

export function addAbortSignal(signal, stream) {
  validateAbortSignal(signal, 'signal');
  if (!isNodeStream(stream)) {
    throw new ERR_INVALID_ARG_TYPE('stream', 'stream.Stream', stream);
  }
  const onAbort = () => {
    stream.destroy(
      new AbortError(undefined, {
        cause: signal.reason,
      })
    );
  };
  if (signal.aborted) {
    onAbort();
  } else {
    signal.addEventListener('abort', onAbort);
    eos(stream, () => signal.removeEventListener('abort', onAbort));
  }
  return stream;
}

// ======================================================================================
// BufferList

export class BufferList {
  head = null;
  tail = null;
  length = 0;

  push(v) {
    const entry = {
      data: v,
      next: null,
    };
    if (this.length > 0) this.tail.next = entry;
    else this.head = entry;
    this.tail = entry;
    ++this.length;
  }
  unshift(v) {
    const entry = {
      data: v,
      next: this.head,
    };
    if (this.length === 0) this.tail = entry;
    this.head = entry;
    ++this.length;
  }
  shift() {
    if (this.length === 0) return;
    const ret = this.head.data;
    if (this.length === 1) this.head = this.tail = null;
    else this.head = this.head.next;
    --this.length;
    return ret;
  }

  clear() {
    this.head = this.tail = null;
    this.length = 0;
  }

  join(s) {
    if (this.length === 0) return '';
    let p = this.head;
    let ret = '' + p.data;
    while ((p = p.next) !== null) ret += s + p.data;
    return ret;
  }

  concat(n) {
    if (this.length === 0) return Buffer.alloc(0);
    const ret = Buffer.allocUnsafe(n >>> 0);
    let p = this.head;
    let i = 0;
    while (p) {
      ret.set(p.data, i);
      i += p.data.length;
      p = p.next;
    }
    return ret;
  }

  consume(n, hasStrings = false) {
    const data = this.head.data;
    if (n < data.length) {
      // `slice` is the same for buffers and strings.
      const slice = data.slice(0, n);
      this.head.data = data.slice(n);
      return slice;
    }
    if (n === data.length) {
      // First chunk is a perfect match.
      return this.shift();
    }
    // Result spans more than one buffer.
    return hasStrings ? this._getString(n) : this._getBuffer(n);
  }

  first() {
    return this.head.data;
  }

  *[Symbol.iterator]() {
    for (let p = this.head; p; p = p.next) {
      yield p.data;
    }
  }

  _getString(n) {
    let ret = '';
    let p = this.head;
    let c = 0;
    do {
      const str = p.data;
      if (n > str.length) {
        ret += str;
        n -= str.length;
      } else {
        if (n === str.length) {
          ret += str;
          ++c;
          if (p.next) this.head = p.next;
          else this.head = this.tail = null;
        } else {
          ret += str.slice(0, n);
          this.head = p;
          p.data = str.slice(n);
        }
        break;
      }
      ++c;
    } while ((p = p.next) !== null);
    this.length -= c;
    return ret;
  }

  _getBuffer(n) {
    const ret = Buffer.allocUnsafe(n);
    const retLen = n;
    let p = this.head;
    let c = 0;
    do {
      const buf = p.data;
      if (n > buf.length) {
        ret.set(buf, retLen - n);
        n -= buf.length;
      } else {
        if (n === buf.length) {
          ret.set(buf, retLen - n);
          ++c;
          if (p.next) this.head = p.next;
          else this.head = this.tail = null;
        } else {
          ret.set(new Uint8Array(buf.buffer, buf.byteOffset, n), retLen - n);
          this.head = p;
          p.data = buf.slice(n);
        }
        break;
      }
      ++c;
    } while ((p = p.next) !== null);
    this.length -= c;
    return ret;
  }
}

// ======================================================================================

// TODO(later): We do not current implement Node.js' Request object. Might never?
function isRequest(stream) {
  return stream && stream.setHeader && typeof stream.abort === 'function';
}

export function eos(stream, options, callback) {
  let _options$readable, _options$writable;
  if (arguments.length === 2) {
    callback = options;
    options = {};
  } else if (options == null) {
    options = {};
  } else {
    validateObject(options, 'options', options);
  }
  validateFunction(callback, 'callback');
  validateAbortSignal(options.signal, 'options.signal');
  callback = once(callback);
  const readable =
    (_options$readable = options.readable) !== null &&
    _options$readable !== undefined
      ? _options$readable
      : isReadableNodeStream(stream);
  const writable =
    (_options$writable = options.writable) !== null &&
    _options$writable !== undefined
      ? _options$writable
      : isWritableNodeStream(stream);
  if (!isNodeStream(stream)) {
    // TODO: Webstreams.
    throw new ERR_INVALID_ARG_TYPE('stream', 'Stream', stream);
  }
  const wState = stream._writableState;
  const rState = stream._readableState;
  const onlegacyfinish = () => {
    if (!stream.writable) {
      onfinish();
    }
  };

  // TODO (ronag): Improve soft detection to include core modules and
  // common ecosystem modules that do properly emit 'close' but fail
  // this generic check.
  let _willEmitClose =
    willEmitClose(stream) &&
    isReadableNodeStream(stream) === readable &&
    isWritableNodeStream(stream) === writable;
  let writableFinished = isWritableFinished(stream, false);
  const onfinish = () => {
    writableFinished = true;
    // Stream should not be destroyed here. If it is that
    // means that user space is doing something differently and
    // we cannot trust willEmitClose.
    if (stream.destroyed) {
      _willEmitClose = false;
    }
    if (_willEmitClose && (!stream.readable || readable)) {
      return;
    }
    if (!readable || readableFinished) {
      callback.call(stream);
    }
  };
  let readableFinished = isReadableFinished(stream, false);
  const onend = () => {
    readableFinished = true;
    // Stream should not be destroyed here. If it is that
    // means that user space is doing something differently and
    // we cannot trust willEmitClose.
    if (stream.destroyed) {
      _willEmitClose = false;
    }
    if (_willEmitClose && (!stream.writable || writable)) {
      return;
    }
    if (!writable || writableFinished) {
      callback.call(stream);
    }
  };
  const onerror = (err) => {
    callback.call(stream, err);
  };
  let closed = isClosed(stream);
  const onclose = () => {
    closed = true;
    const errored = isWritableErrored(stream) || isReadableErrored(stream);
    if (errored && typeof errored !== 'boolean') {
      return callback.call(stream, errored);
    }
    if (readable && !readableFinished && isReadableNodeStream(stream, true)) {
      if (!isReadableFinished(stream, false))
        return callback.call(stream, new ERR_STREAM_PREMATURE_CLOSE());
    }
    if (writable && !writableFinished) {
      if (!isWritableFinished(stream, false))
        return callback.call(stream, new ERR_STREAM_PREMATURE_CLOSE());
    }
    callback.call(stream);
  };
  const onrequest = () => {
    stream.req.on('finish', onfinish);
  };
  if (isRequest(stream)) {
    stream.on('complete', onfinish);
    if (!_willEmitClose) {
      stream.on('abort', onclose);
    }
    if (stream.req) {
      onrequest();
    } else {
      stream.on('request', onrequest);
    }
  } else if (writable && !wState) {
    // legacy streams
    stream.on('end', onlegacyfinish);
    stream.on('close', onlegacyfinish);
  }

  // Not all streams will emit 'close' after 'aborted'.
  if (!_willEmitClose && typeof stream.aborted === 'boolean') {
    stream.on('aborted', onclose);
  }
  stream.on('end', onend);
  stream.on('finish', onfinish);
  if (options.error !== false) {
    stream.on('error', onerror);
  }
  stream.on('close', onclose);
  if (closed) {
    process.nextTick(onclose);
  } else if (
    (wState !== null && wState !== undefined && wState.errorEmitted) ||
    (rState !== null && rState !== undefined && rState.errorEmitted)
  ) {
    if (!_willEmitClose) {
      process.nextTick(onclose);
    }
  } else if (
    !readable &&
    (!_willEmitClose || isReadable(stream)) &&
    (writableFinished || isWritable(stream) === false)
  ) {
    process.nextTick(onclose);
  } else if (
    !writable &&
    (!_willEmitClose || isWritable(stream)) &&
    (readableFinished || isReadable(stream) === false)
  ) {
    process.nextTick(onclose);
  } else if (rState && stream.req && stream.aborted) {
    process.nextTick(onclose);
  }
  const cleanup = () => {
    callback = nop;
    stream.removeListener('aborted', onclose);
    stream.removeListener('complete', onfinish);
    stream.removeListener('abort', onclose);
    stream.removeListener('request', onrequest);
    if (stream.req) stream.req.removeListener('finish', onfinish);
    stream.removeListener('end', onlegacyfinish);
    stream.removeListener('close', onlegacyfinish);
    stream.removeListener('finish', onfinish);
    stream.removeListener('end', onend);
    stream.removeListener('error', onerror);
    stream.removeListener('close', onclose);
  };
  if (options.signal && !closed) {
    const abort = () => {
      // Keep it because cleanup removes it.
      const endCallback = callback;
      cleanup();
      endCallback.call(
        stream,
        new AbortError(undefined, {
          cause: options.signal?.reason,
        })
      );
    };
    if (options.signal.aborted) {
      process.nextTick(abort);
    } else {
      const originalCallback = callback;
      callback = once((...args) => {
        options.signal.removeEventListener('abort', abort);
        originalCallback.apply(stream, args);
      });
      options.signal.addEventListener('abort', abort);
    }
  }
  return cleanup;
}

export function finished(stream, opts) {
  return new Promise((resolve, reject) => {
    eos(stream, opts, (err) => {
      if (err) {
        reject(err);
      } else {
        resolve();
      }
    });
  });
}

eos.finished = finished;

// ======================================================================================
// Destroy

function checkError(err, w, r) {
  if (err) {
    // Avoid V8 leak, https://github.com/nodejs/node/pull/34103#issuecomment-652002364
    err.stack; // eslint-disable-line no-unused-expressions

    if (w && !w.errored) {
      w.errored = err;
    }
    if (r && !r.errored) {
      r.errored = err;
    }
  }
}

export function destroy(err, cb) {
  const r = this._readableState;
  const w = this._writableState;
  // With duplex streams we use the writable side for state.
  const s = w || r;
  if ((w && w.destroyed) || (r && r.destroyed)) {
    if (typeof cb === 'function') {
      cb();
    }
    return this;
  }

  // We set destroyed to true before firing error callbacks in order
  // to make it re-entrance safe in case destroy() is called within callbacks
  checkError(err, w, r);
  if (w) {
    w.destroyed = true;
  }
  if (r) {
    r.destroyed = true;
  }

  // If still constructing then defer calling _destroy.
  if (!s.constructed) {
    this.once(kDestroy, function (er) {
      _destroy(this, aggregateTwoErrors(er, err), cb);
    });
  } else {
    _destroy(this, err, cb);
  }
  return this;
}

function _destroy(self, err, cb) {
  let called = false;
  function onDestroy(err) {
    if (called) {
      return;
    }
    called = true;
    const r = self._readableState;
    const w = self._writableState;
    checkError(err, w, r);
    if (w) {
      w.closed = true;
    }
    if (r) {
      r.closed = true;
    }
    if (typeof cb === 'function') {
      cb(err);
    }
    if (err) {
      process.nextTick(emitErrorCloseNT, self, err);
    } else {
      process.nextTick(emitCloseNT, self);
    }
  }
  try {
    self._destroy(err || null, onDestroy);
  } catch (err) {
    onDestroy(err);
  }
}

function emitErrorCloseNT(self, err) {
  emitErrorNT(self, err);
  emitCloseNT(self);
}

function emitCloseNT(self) {
  const r = self._readableState;
  const w = self._writableState;
  if (w) {
    w.closeEmitted = true;
  }
  if (r) {
    r.closeEmitted = true;
  }
  if ((w && w.emitClose) || (r && r.emitClose)) {
    self.emit('close');
  }
}

function emitErrorNT(self, err) {
  const r = self._readableState;
  const w = self._writableState;
  if ((w && w.errorEmitted) || (r && r.errorEmitted)) {
    return;
  }
  if (w) {
    w.errorEmitted = true;
  }
  if (r) {
    r.errorEmitted = true;
  }
  self.emit('error', err);
}

export function undestroy() {
  const r = this._readableState;
  const w = this._writableState;
  if (r) {
    r.constructed = true;
    r.closed = false;
    r.closeEmitted = false;
    r.destroyed = false;
    r.errored = null;
    r.errorEmitted = false;
    r.reading = false;
    r.ended = r.readable === false;
    r.endEmitted = r.readable === false;
  }
  if (w) {
    w.constructed = true;
    w.destroyed = false;
    w.closed = false;
    w.closeEmitted = false;
    w.errored = null;
    w.errorEmitted = false;
    w.finalCalled = false;
    w.prefinished = false;
    w.ended = w.writable === false;
    w.ending = w.writable === false;
    w.finished = w.writable === false;
  }
}

export function errorOrDestroy(stream, err, sync = false) {
  // We have tests that rely on errors being emitted
  // in the same tick, so changing this is semver major.
  // For now when you opt-in to autoDestroy we allow
  // the error to be emitted nextTick. In a future
  // semver major update we should change the default to this.

  const r = stream._readableState;
  const w = stream._writableState;
  if ((w && w.destroyed) || (r && r.destroyed)) {
    return;
  }
  if ((r && r.autoDestroy) || (w && w.autoDestroy)) stream.destroy(err);
  else if (err) {
    // Avoid V8 leak, https://github.com/nodejs/node/pull/34103#issuecomment-652002364
    err.stack; // eslint-disable-line no-unused-expressions

    if (w && !w.errored) {
      w.errored = err;
    }
    if (r && !r.errored) {
      r.errored = err;
    }
    if (sync) {
      process.nextTick(emitErrorNT, stream, err);
    } else {
      emitErrorNT(stream, err);
    }
  }
}

export function construct(stream, cb) {
  if (typeof stream._construct !== 'function') {
    return;
  }
  const r = stream._readableState;
  const w = stream._writableState;
  if (r) {
    r.constructed = false;
  }
  if (w) {
    w.constructed = false;
  }
  stream.once(kConstruct, cb);
  if (stream.listenerCount(kConstruct) > 1) {
    // Duplex
    return;
  }
  process.nextTick(constructNT, stream);
}

function constructNT(stream) {
  let called = false;
  function onConstruct(err) {
    if (called) {
      errorOrDestroy(
        stream,
        err !== null && err !== undefined ? err : new ERR_MULTIPLE_CALLBACK()
      );
      return;
    }
    called = true;
    const r = stream._readableState;
    const w = stream._writableState;
    const s = w || r;
    if (r) {
      r.constructed = true;
    }
    if (w) {
      w.constructed = true;
    }
    if (s.destroyed) {
      stream.emit(kDestroy, err);
    } else if (err) {
      errorOrDestroy(stream, err, true);
    } else {
      process.nextTick(emitConstructNT, stream);
    }
  }
  try {
    stream._construct(onConstruct);
  } catch (err) {
    onConstruct(err);
  }
}

function emitConstructNT(stream) {
  stream.emit(kConstruct);
}

function emitCloseLegacy(stream) {
  stream.emit('close');
}

function emitErrorCloseLegacy(stream, err) {
  stream.emit('error', err);
  process.nextTick(emitCloseLegacy, stream);
}

// Normalize destroy for legacy.
export function destroyer(stream, err) {
  if (!stream || isDestroyed(stream)) {
    return;
  }
  if (!err && !isFinished(stream)) {
    err = new AbortError();
  }

  // TODO: Remove isRequest branches.
  if (isServerRequest(stream)) {
    stream.socket = null;
    stream.destroy(err);
  } else if (isRequest(stream)) {
    stream.abort();
  } else if (isRequest(stream.req)) {
    stream.req.abort();
  } else if (typeof stream.destroy === 'function') {
    stream.destroy(err);
  } else if (typeof stream.close === 'function') {
    // TODO: Don't lose err?
    stream.close();
  } else if (err) {
    process.nextTick(emitErrorCloseLegacy, stream, err);
  } else {
    process.nextTick(emitCloseLegacy, stream);
  }
  if (!stream.destroyed) {
    stream[kDestroyed] = true;
  }
}
