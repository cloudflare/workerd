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

/* eslint-disable @typescript-eslint/no-unsafe-member-access,@typescript-eslint/no-unsafe-assignment,@typescript-eslint/restrict-plus-operands,@typescript-eslint/no-unsafe-return,@typescript-eslint/no-unnecessary-condition,@typescript-eslint/no-unnecessary-type-conversion,@typescript-eslint/no-unnecessary-boolean-literal-compare,no-var */

import { Buffer } from 'node-internal:internal_buffer';

import type { Readable } from 'node-internal:streams_readable';
import type { Writable } from 'node-internal:streams_writable';
import type { Transform } from 'node-internal:streams_transform';
import type { Duplex } from 'node-internal:streams_duplex';
import type { OutgoingMessage } from 'node-internal:internal_http_outgoing';
import type { ServerResponse } from 'node-internal:internal_http_server';

export const kDestroyed = Symbol('kDestroyed');
export const kIsErrored = Symbol('kIsErrored');
export const kIsReadable = Symbol('kIsReadable');
export const kIsDisturbed = Symbol('kIsDisturbed');
export const kPaused = Symbol('kPaused');
export const kOnFinished = Symbol('kOnFinished');

type NodeStream = Readable | Writable | Transform | Duplex;

export const kIsDestroyed = Symbol.for('nodejs.stream.destroyed');
export const kIsWritable = Symbol.for('nodejs.stream.writable');
export const kOnConstructed = Symbol('kOnConstructed');

export function isReadableNodeStream(
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  obj: any,
  strict: boolean = false
): obj is Readable {
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

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function isWritableNodeStream(obj: any): obj is Writable {
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

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function isDuplexNodeStream(obj: any): obj is Duplex {
  return !!(
    obj &&
    typeof obj.pipe === 'function' &&
    obj._readableState &&
    typeof obj.on === 'function' &&
    typeof obj.write === 'function'
  );
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function isNodeStream(obj: any): obj is NodeStream {
  return (
    obj &&
    (obj._readableState ||
      obj._writableState ||
      (typeof obj.write === 'function' && typeof obj.on === 'function') ||
      (typeof obj.pipe === 'function' && typeof obj.on === 'function'))
  );
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function isReadableStream(obj: any): obj is ReadableStream {
  return !!(
    obj &&
    !isNodeStream(obj) &&
    typeof obj.pipeThrough === 'function' &&
    typeof obj.getReader === 'function' &&
    typeof obj.cancel === 'function'
  );
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function isWritableStream(obj: any): obj is WritableStream {
  return !!(
    obj &&
    !isNodeStream(obj) &&
    typeof obj.getWriter === 'function' &&
    typeof obj.abort === 'function'
  );
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function isIterable(obj: any, isAsync?: true | false): boolean {
  if (obj == null) return false;
  if (isAsync === true) return typeof obj[Symbol.asyncIterator] === 'function';
  if (isAsync === false) return typeof obj[Symbol.iterator] === 'function';
  return (
    typeof obj[Symbol.asyncIterator] === 'function' ||
    typeof obj[Symbol.iterator] === 'function'
  );
}

export function isDestroyed(stream: unknown): boolean | null {
  if (!isNodeStream(stream)) return null;
  const wState = stream._writableState;
  const rState = stream._readableState;
  const state = wState || rState;
  return !!(stream.destroyed || stream[kDestroyed] || state?.destroyed);
}

export function isWritableEnded(stream: Writable): boolean | null {
  if (!isWritableNodeStream(stream)) return null;
  if (stream.writableEnded === true) return true;
  const wState = stream._writableState;
  if (wState != null && wState.errored) return false;
  if (typeof (wState == null ? undefined : wState.ended) !== 'boolean')
    return null;
  return wState.ended ?? false;
}

export function isWritableFinished(
  stream: NodeStream,
  strict = false
): boolean | null {
  if (!isWritableNodeStream(stream)) return null;
  if (stream.writableFinished === true) return true;
  const wState = stream._writableState;
  if (wState != null && wState.errored) return false;
  if (typeof (wState == null ? undefined : wState.finished) !== 'boolean')
    return null;
  return !!(
    wState.finished ||
    (strict === false && wState.ended === true && wState.length === 0)
  );
}

export function isReadableEnded(stream: NodeStream): boolean | null {
  if (!isReadableNodeStream(stream)) return null;
  if (stream.readableEnded === true) return true;
  const rState = stream._readableState;
  if (!rState || rState.errored) return false;
  if (typeof (rState == null ? undefined : rState.ended) !== 'boolean')
    return null;
  return rState.ended ?? false;
}

export function isReadableFinished(
  stream: Readable,
  strict = false
): boolean | null {
  if (!isReadableNodeStream(stream)) return null;
  const rState = stream._readableState;
  if (rState != null && rState.errored) return false;
  if (typeof (rState == null ? undefined : rState.endEmitted) !== 'boolean')
    return null;
  return !!(
    rState.endEmitted ||
    (strict === false && rState.ended === true && rState.length === 0)
  );
}

export function isReadable(stream: NodeStream): boolean | null {
  if (stream && stream[kIsReadable] != null) return stream[kIsReadable];
  if (typeof (stream == null ? undefined : stream.readable) !== 'boolean')
    return null;
  if (isDestroyed(stream)) return false;
  return (
    isReadableNodeStream(stream) &&
    stream.readable &&
    !isReadableFinished(stream)
  );
}

export function isWritable(stream: NodeStream): boolean | null {
  if (typeof (stream == null ? undefined : stream.writable) !== 'boolean')
    return null;
  if (isDestroyed(stream)) return false;
  return (
    isWritableNodeStream(stream) && stream.writable && !isWritableEnded(stream)
  );
}

export function isFinished(
  stream: unknown,
  opts: { readable?: boolean; writable?: boolean } = {}
): boolean | null {
  if (!isNodeStream(stream)) {
    return null;
  }
  if (isDestroyed(stream)) {
    return true;
  }
  if (
    (opts == null ? undefined : opts.readable) !== false &&
    isReadable(stream)
  ) {
    return false;
  }
  if (
    (opts == null ? undefined : opts.writable) !== false &&
    isWritable(stream)
  ) {
    return false;
  }
  return true;
}

export function isWritableErrored(stream: unknown): Error | boolean | null {
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

export function isReadableErrored(stream: unknown): Error | boolean | null {
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

export function isClosed(stream: unknown): boolean | null | undefined {
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
export function isOutgoingMessage(stream: unknown): stream is OutgoingMessage {
  return (
    stream != null &&
    typeof stream === 'object' &&
    '_closed' in stream &&
    '_defaultKeepAlive' in stream &&
    '_removedConnection' in stream &&
    '_removedContLen' in stream &&
    typeof stream._closed === 'boolean' &&
    typeof stream._defaultKeepAlive === 'boolean' &&
    typeof stream._removedConnection === 'boolean' &&
    typeof stream._removedContLen === 'boolean'
  );
}

// TODO(later): We do not actually support Server Response yet. Might not ever?
// Keeping this here tho just to keep things simple.
export function isServerResponse(stream: unknown): stream is ServerResponse {
  return (
    stream != null &&
    typeof stream === 'object' &&
    '_sent100' in stream &&
    typeof stream._sent100 === 'boolean' &&
    isOutgoingMessage(stream)
  );
}

// TODO(later): We do not actually support Server Request yet. Might not ever?
// Keeping this here tho just to keep things simple.
// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function isServerRequest(stream: any): boolean {
  let _stream$req;
  return (
    typeof stream._consuming === 'boolean' &&
    typeof stream._dumped === 'boolean' &&
    ((_stream$req = stream.req) === null || _stream$req === undefined
      ? undefined
      : _stream$req.upgradeOrConnect) === undefined
  );
}

export function willEmitClose(stream: unknown): boolean | null {
  if (!isNodeStream(stream)) return null;
  const wState = stream._writableState;
  const rState = stream._readableState;
  const state = wState || rState;
  return (
    (!state && isServerResponse(stream)) ||
    !!(state && state.autoDestroy && state.emitClose && state.closed === false)
  );
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function isDisturbed(stream: any): boolean {
  let _stream$kIsDisturbed;
  return !!(
    stream &&
    ((_stream$kIsDisturbed = stream[kIsDisturbed]) !== null &&
    _stream$kIsDisturbed !== undefined
      ? _stream$kIsDisturbed
      : stream.readableDidRead || stream.readableAborted)
  );
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function isErrored(stream: any): boolean {
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

export const nop = (): void => {};

type CallbackFunction = (...args: unknown[]) => void;

export function once<T extends CallbackFunction>(callback: T): T {
  let called = false;
  return function (this: unknown, ...args: unknown[]) {
    if (called) {
      return;
    }
    called = true;
    callback.apply(this, args);
  } as T;
}

// ======================================================================================
// BufferList

export type BufferListEntry = {
  data: Buffer;
  next: BufferListEntry | null;
};

export class BufferList {
  head: BufferListEntry | null = null;
  tail: BufferListEntry | null = null;
  length = 0;

  push(v: Buffer): void {
    const entry = {
      data: v,
      next: null,
    };
    if (this.length > 0 && this.tail != null) this.tail.next = entry;
    else this.head = entry;
    this.tail = entry;
    ++this.length;
  }
  unshift(v: Buffer): void {
    const entry = {
      data: v,
      next: this.head,
    };
    if (this.length === 0) this.tail = entry;
    this.head = entry;
    ++this.length;
  }
  shift(): Buffer | undefined {
    if (this.length === 0) return;
    const ret = this.head?.data as Buffer;
    if (this.length === 1) this.head = this.tail = null;
    else this.head = this.head?.next ?? null;
    --this.length;
    return ret;
  }

  clear(): void {
    this.head = this.tail = null;
    this.length = 0;
  }

  join(s: Buffer): string {
    if (this.length === 0) return '';
    let p = this.head;
    let ret = '' + p?.data;
    while ((p = p?.next ?? null) != null) ret += s + String(p.data);
    return ret;
  }

  concat(n: number): Buffer {
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

  consume(n: number, hasStrings: boolean = false): Buffer | string | undefined {
    const data = this.head?.data as Buffer;
    if (n < data.length) {
      // `slice` is the same for buffers and strings.
      const slice = data.slice(0, n);
      if (this.head != null) {
        this.head.data = data.slice(n) as Buffer;
      }
      return slice as Buffer;
    }
    if (n === data.length) {
      // First chunk is a perfect match.
      return this.shift();
    }
    // Result spans more than one buffer.
    return hasStrings ? this._getString(n) : this._getBuffer(n);
  }

  first(): Buffer | null {
    return this.head?.data ?? null;
  }

  *[Symbol.iterator](): Generator<Buffer | string, void, unknown> {
    for (let p = this.head; p; p = p.next) {
      yield p.data;
    }
  }

  _getString(n: number): string {
    let ret = '';
    let p = this.head;
    let c = 0;
    do {
      const str = p?.data as Buffer;
      if (n > str.length) {
        ret += str;
        n -= str.length;
      } else {
        if (n === str.length) {
          ret += str;
          ++c;
          if (p?.next) this.head = p.next;
          else this.head = this.tail = null;
        } else {
          ret += str.slice(0, n);
          this.head = p;
          if (p != null) {
            p.data = str.slice(n) as Buffer;
          }
        }
        break;
      }
      ++c;
    } while ((p = p?.next ?? null) != null);
    this.length -= c;
    return ret;
  }

  _getBuffer(n: number): Buffer {
    const ret = Buffer.allocUnsafe(n);
    const retLen = n;
    let p = this.head;
    let c = 0;
    do {
      const buf = p?.data as Buffer;
      if (n > buf.length) {
        ret.set(buf, retLen - n);
        n -= buf.length;
      } else {
        if (n === buf.length) {
          ret.set(buf, retLen - n);
          ++c;
          if (p?.next) this.head = p.next;
          else this.head = this.tail = null;
        } else {
          ret.set(new Uint8Array(buf.buffer, buf.byteOffset, n), retLen - n);
          this.head = p;
          if (p != null) {
            p.data = buf.slice(n) as Buffer;
          }
        }
        break;
      }
      ++c;
    } while ((p = p?.next ?? null) != null);
    this.length -= c;
    return ret;
  }
}
