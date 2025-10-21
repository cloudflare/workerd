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

/* eslint-disable @typescript-eslint/no-empty-object-type */

import inner from 'cloudflare-internal:sockets';

import {
  AbortError,
  ERR_INVALID_ARG_VALUE,
  ERR_INVALID_ARG_TYPE,
  ERR_MISSING_ARGS,
  ERR_OUT_OF_RANGE,
  ERR_OPTION_NOT_IMPLEMENTED,
  ERR_SOCKET_CLOSED,
  ERR_SOCKET_CLOSED_BEFORE_CONNECTION,
  ERR_SOCKET_CONNECTING,
  ERR_INVALID_IP_ADDRESS,
  ERR_INVALID_ADDRESS,
  EPIPE,
} from 'node-internal:internal_errors';

import {
  validateAbortSignal,
  validateArray,
  validateFunction,
  validateInt32,
  validateNumber,
  validatePort,
  validateObject,
  validateOneOf,
  validateBoolean,
  validateString,
  validateUint32,
} from 'node-internal:validators';

import { isUint8Array, isArrayBufferView } from 'node-internal:internal_types';
import { Duplex } from 'node-internal:streams_duplex';
import { Buffer } from 'node-internal:internal_buffer';
import type {
  IpcSocketConnectOpts,
  SocketConnectOpts,
  TcpSocketConnectOpts,
  AddressInfo,
  Socket as _Socket,
  SocketAddress as _SocketAddress,
  SocketAddressInitOptions,
  IPVersion,
  OnReadOpts,
} from 'node:net';
import type { InspectOptions } from 'node:util';
import type { Writable } from 'node:stream';
import { JSStreamSocket } from 'node-internal:internal_tls_jsstream';

import { inspect } from 'node-internal:internal_inspect';
import type { FixedLengthArray } from 'node-internal:internal_utils';

const kInspect = inspect.custom;

const kLastWriteQueueSize = Symbol('kLastWriteQueueSize');
const kTimeout = Symbol('kTimeout');
const kBuffer = Symbol('kBuffer');
const kBufferCb = Symbol('kBufferCb');
const kBufferGen = Symbol('kBufferGen');
const kBytesRead = Symbol('kBytesRead');
const kBytesWritten = Symbol('kBytesWritten');
const kUpdateTimer = Symbol('kUpdateTimer');
export const normalizedArgsSymbol = Symbol('normalizedArgs');
export const kReinitializeHandle = Symbol('kReinitializeHandle');

// Once the socket has been opened, the socket info provided by the
// socket.opened promise will be stored here.
const kSocketInfo = Symbol('kSocketInfo');

// IPv4 Segment
const v4Seg = '(?:25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9])';
const v4Str = `(?:${v4Seg}\\.){3}${v4Seg}`;
const IPv4Reg = new RegExp(`^${v4Str}$`);

// IPv6 Segment
const v6Seg = '(?:[0-9a-fA-F]{1,4})';
const IPv6Reg = new RegExp(
  '^(?:' +
    `(?:${v6Seg}:){7}(?:${v6Seg}|:)|` +
    `(?:${v6Seg}:){6}(?:${v4Str}|:${v6Seg}|:)|` +
    `(?:${v6Seg}:){5}(?::${v4Str}|(?::${v6Seg}){1,2}|:)|` +
    `(?:${v6Seg}:){4}(?:(?::${v6Seg}){0,1}:${v4Str}|(?::${v6Seg}){1,3}|:)|` +
    `(?:${v6Seg}:){3}(?:(?::${v6Seg}){0,2}:${v4Str}|(?::${v6Seg}){1,4}|:)|` +
    `(?:${v6Seg}:){2}(?:(?::${v6Seg}){0,3}:${v4Str}|(?::${v6Seg}){1,5}|:)|` +
    `(?:${v6Seg}:){1}(?:(?::${v6Seg}){0,4}:${v4Str}|(?::${v6Seg}){1,6}|:)|` +
    `(?::(?:(?::${v6Seg}){0,5}:${v4Str}|(?::${v6Seg}){1,7}|:))` +
    ')(?:%[0-9a-zA-Z-.:]{1,})?$'
);

const TIMEOUT_MAX = 2 ** 31 - 1;

// ======================================================================================

export type SocketOptions = {
  timeout?: number;
  writable?: boolean;
  readable?: boolean;
  decodeStrings?: boolean;
  autoDestroy?: boolean;
  objectMode?: boolean;
  readableObjectMode?: boolean;
  writableObjectMode?: boolean;
  keepAliveInitialDelay?: number;
  fd?: number;
  handle?: Socket['_handle'];
  noDelay?: boolean;
  keepAlive?: boolean;
  allowHalfOpen?: boolean | undefined;
  emitClose?: boolean;
  signal?: AbortSignal | undefined;
  onread?:
    | ({ callback?: () => Uint8Array; buffer?: Uint8Array } & OnReadOpts)
    | null
    | undefined;
};

export function Server(): void {
  throw new Error('Server is not implemented');
}

export type SocketWriteData = Array<{
  chunk: any; // eslint-disable-line @typescript-eslint/no-explicit-any
  encoding: BufferEncoding;
}>;

// @ts-expect-error TS2323 Redeclare error.
export declare class Socket extends _Socket {
  timeout: number;
  connecting: boolean;
  _aborted: boolean;
  _hadError: boolean;
  _parent: null | Socket['_handle'];
  _parentWrap: null | Socket | JSStreamSocket;
  _host: null | string;
  _peername: null | string;
  _getsockname(): {
    address?: string;
    port?: number;
    family?: string;
  };
  [kLastWriteQueueSize]: number | null | undefined;
  [kTimeout]: Socket | null | undefined;
  [kBuffer]: null | boolean | Uint8Array;
  [kBufferCb]:
    | null
    | undefined
    | ((len?: number, buf?: Buffer) => boolean | Uint8Array);
  [kBufferGen]: null | (() => undefined | Uint8Array);
  [kSocketInfo]: null | {
    address?: string;
    port?: number;
    family?: number | string;
    remoteAddress?: Record<string, unknown>;
  };
  [kBytesRead]: number;
  [kBytesWritten]: number;
  [kReinitializeHandle](handle: Socket['_handle']): void;
  _closeAfterHandlingError: boolean;
  _handle: null | {
    writeQueueSize?: number;
    lastWriteQueueSize?: number;
    reading: boolean | undefined;
    bytesRead: number;
    bytesWritten: number;
    socket: ReturnType<typeof inner.connect>;
    reader: ReadableStreamBYOBReader;
    writer: WritableStreamDefaultWriter<unknown>;
    options: {
      host: string;
      port: number;
      addressType: number;
    };
  };
  _sockname?: null | AddressInfo;
  _onTimeout(): void;
  _unrefTimer(): void;
  _writeGeneric(
    writev: boolean,
    data: SocketWriteData,
    encoding: string,
    cb: (err?: Error) => void
  ): void;
  _final(cb: (err?: Error) => void): void;
  _read(n: number): void;
  _reset(): void;
  _getpeername(): Record<string, unknown>;
  _writableState: null | unknown[];
  _bytesDispatched: number;
  _pendingData: SocketWriteData | null;
  _pendingEncoding: string;
  _undestroy(): void;
  // This should have existed in _Socket type but it's not...
  writableBuffer?: Writable & {
    allBuffers: boolean;
    length: number;
  };

  // Defined by TLSSocket
  encrypted?: boolean;
  _finishInit(): void;

  constructor(options?: SocketOptions);
  prototype: Socket;
  resetAndClosing?: boolean;
}

// @ts-expect-error TS2323 Redeclare error.
export function Socket(this: Socket, options?: SocketOptions): Socket {
  if (!(this instanceof Socket)) {
    return new Socket(options);
  }

  if (options?.objectMode) {
    throw new ERR_INVALID_ARG_VALUE(
      'options.objectMode',
      options.objectMode,
      'is not supported'
    );
  } else if (options?.readableObjectMode || options?.writableObjectMode) {
    throw new ERR_INVALID_ARG_VALUE(
      `options.${
        options.readableObjectMode ? 'readableObjectMode' : 'writableObjectMode'
      }`,
      options.readableObjectMode || options.writableObjectMode,
      'is not supported'
    );
  }
  if (options?.keepAliveInitialDelay !== undefined) {
    validateNumber(
      options.keepAliveInitialDelay,
      'options.keepAliveInitialDelay'
    );

    if (options.keepAliveInitialDelay < 0) {
      options.keepAliveInitialDelay = 0;
    }
  }

  if (typeof options === 'number') {
    options = { fd: options as number };
  } else {
    options = { ...options };
  }

  if (options.fd !== undefined) {
    // We are not supporting the options.fd option for now. This is the option
    // that allows users to pass in a file descriptor to an existing socket.
    // Workers doesn't have file descriptors and does not use them in any way.
    throw new ERR_OPTION_NOT_IMPLEMENTED('options.fd');
  }

  // We do not support the noDelay and keepAlive options at this
  // time and will just ignore them if they are passed.
  //
  // We shouldn't throw an error for the following validations,
  // because it breaks packages such as redis.
  //
  // if (options.noDelay) {
  //   throw new ERR_OPTION_NOT_IMPLEMENTED('truthy options.noDelay');
  // }
  //
  // if (options.keepAlive) {
  //   throw new ERR_OPTION_NOT_IMPLEMENTED('truthy options.keepAlive');
  // }

  options.allowHalfOpen = Boolean(options.allowHalfOpen);
  // TODO(now): Match behavior with Node.js
  // In Node.js, emitClose and autoDestroy are false by default so that
  // the socket must handle those itself, including emitting the close
  // event with the hadError argument. We should match that behavior.
  options.emitClose = false;
  options.autoDestroy = true;
  options.decodeStrings = false;

  // In Node.js, these are meaningful when the options.fd is used.
  // We do not support options.fd so we just ignore whatever value
  // is given and always pass true.
  options.readable = true;
  options.writable = true;

  this._handle = null;
  this.connecting = false;
  this._hadError = false;
  this._parent = null;
  this._parentWrap = null;
  this._host = null;
  this[kLastWriteQueueSize] = 0;
  this[kTimeout] = null;
  this[kBuffer] = null;
  this[kBufferCb] = null;
  this[kBufferGen] = null;
  this[kSocketInfo] = null;
  this[kBytesRead] = 0;
  this[kBytesWritten] = 0;
  this._closeAfterHandlingError = false;
  // @ts-expect-error TS2540 Required due to types
  this.autoSelectFamilyAttemptedAddresses = [];

  this._undestroy();
  this._sockname = null;
  this._pendingData = null;
  this._pendingEncoding = '';

  // Call Duplex constructor before setting up the abort signal
  // This ensures the stream methods are properly set up before
  // any abort handling that might call destroy()
  // @ts-expect-error TS2379 Type incompatibility with exactOptionalPropertyTypes
  Duplex.call(this, options);

  if (options.handle) {
    validateObject(options.handle, 'options.handle');
    this._handle = options.handle;
  }

  // We explicitly listen for all 'end' events, not only for once
  // because Socket class supports reconnection through s.connect();
  this.on('end', onReadableStreamEnd);

  const onread = options.onread;
  if (
    onread != null &&
    typeof onread === 'object' &&
    // The onread.buffer can either be a Uint8Array or a function that returns
    // a Uint8Array.
    (isUint8Array(onread.buffer) || typeof onread.buffer === 'function') &&
    // The onread.callback is the function used to deliver the read buffer to
    // the application.
    typeof onread.callback === 'function'
  ) {
    if (typeof onread.buffer === 'function') {
      this[kBuffer] = true;
      this[kBufferGen] = onread.buffer;
    } else {
      this[kBuffer] = onread.buffer;
      this[kBufferGen] = (): Uint8Array | undefined => onread.buffer;
    }
    this[kBufferCb] = onread.callback;
  } else {
    this[kBuffer] = true;
    this[kBufferGen] = (): Uint8Array => new Uint8Array(4096);
    this[kBufferCb] = undefined;
  }

  // Now set up abort signal handling after the Duplex constructor
  if (options.signal) {
    addClientAbortSignalOption(this, options.signal);
  }

  this[kBytesRead] = 0;
  this[kBytesWritten] = 0;

  // TODO(soon): Enable this once blockList is implemented.
  // if (options.blockList) {
  //     if (!BlockList.isBlockList(options.blockList)) {
  //       throw new ERR_INVALID_ARG_TYPE('options.blockList', 'net.BlockList', options.blockList);
  //     }
  //     this.blockList = options.blockList;
  //   }

  return this;
}

Object.setPrototypeOf(Socket.prototype, Duplex.prototype);
Object.setPrototypeOf(Socket, Duplex);

Socket.prototype._unrefTimer = function _unrefTimer(this: Socket): void {
  // eslint-disable-next-line @typescript-eslint/no-this-alias
  for (let s: Socket | null = this; s != null; s = s._parentWrap) {
    if (s[kTimeout] != null) {
      clearTimeout(s[kTimeout] as unknown as number);
      s[kTimeout] = this.setTimeout(s.timeout, (): void => {
        s._onTimeout();
      });
    }
  }
};

Socket.prototype.setTimeout = function (
  this: Socket,
  msecs: number,
  callback?: () => void
): Socket {
  if (this.destroyed) return this;

  this.timeout = msecs;

  // Type checking identical to timers.enroll()
  msecs = getTimerDuration(msecs, 'msecs');

  // Attempt to clear an existing timer in both cases -
  // even if it will be rescheduled we don't want to leak an existing timer.
  clearTimeout(this[kTimeout] as unknown as number);

  if (msecs === 0) {
    if (callback !== undefined) {
      validateFunction(callback, 'callback');
      this.removeListener('timeout', callback);
    }
  } else {
    // @ts-expect-error TS2740 Required to not overcomplicate types
    this[kTimeout] = setTimeout((): void => {
      this._onTimeout();
    }, msecs);
    if (callback !== undefined) {
      validateFunction(callback, 'callback');
      this.once('timeout', callback);
    }
  }
  return this;
};

Socket.prototype._onTimeout = function (this: Socket): void {
  const handle = this._handle;
  const lastWriteQueueSize = this[kLastWriteQueueSize] as number;
  if (lastWriteQueueSize > 0 && handle) {
    // `lastWriteQueueSize !== writeQueueSize` means there is
    // an active write in progress, so we suppress the timeout.
    const { writeQueueSize } = handle;
    if (lastWriteQueueSize !== writeQueueSize) {
      this[kLastWriteQueueSize] = writeQueueSize;
      this._unrefTimer();
      return;
    }
  }
  this.emit('timeout');
};

Socket.prototype._getpeername = function (
  this: Socket
): Record<string, unknown> {
  if (this._handle == null || this[kSocketInfo] == null) {
    return {};
  }

  return { ...this[kSocketInfo].remoteAddress };
};

Socket.prototype._getsockname = function (this: Socket): AddressInfo | {} {
  if (this._handle == null) {
    return {};
  }
  this._sockname ??= {
    address: '0.0.0.0',
    port: 0,
    family: 'IPv4',
  };
  return this._sockname;
};

Socket.prototype.address = function (this: Socket): {} | AddressInfo {
  return this._getsockname();
};

// ======================================================================================
// Writable side ...

Socket.prototype._writeGeneric = function (
  this: Socket,
  writev: boolean,
  data: SocketWriteData,
  encoding: string,
  cb: (err?: Error) => void
  // eslint-disable-next-line @typescript-eslint/no-invalid-void-type
): false | void {
  // If we are still connecting, buffer this for later.
  // The writable logic will buffer up any more writes while
  // waiting for this one to be done.
  try {
    if (this.connecting) {
      this._pendingData = data;
      this._pendingEncoding = encoding;
      function onClose(): void {
        cb(new ERR_SOCKET_CLOSED_BEFORE_CONNECTION());
      }
      this.once('connect', () => {
        this.off('close', onClose);
        this._writeGeneric(writev, data, encoding, cb);
      });
      this.once('close', onClose);
      return;
    }

    this._pendingData = null;
    this._pendingEncoding = '';

    if (this._handle?.writer === undefined) {
      cb(new ERR_SOCKET_CLOSED());
      return false;
    }

    this._unrefTimer();

    let lastWriteSize = 0;
    if (writev) {
      // data is an array of strings or ArrayBufferViews. We're going to concat
      // them all together into a single buffer so we can write all at once. This
      // trades higher memory use by copying the input buffers for fewer round trips
      // through the write loop in the stream. Since the write loops involve bouncing
      // back and forth across the kj event loop boundary and requires reacquiring the
      // isolate lock after each write, this should be more efficient in the long run.
      const buffers = [];
      for (const d of data) {
        if (typeof d.chunk === 'string') {
          const buf = Buffer.from(d.chunk, d.encoding);
          buffers.push(buf);
          lastWriteSize += buf.byteLength;
        } else if (isArrayBufferView(d.chunk)) {
          buffers.push(
            new Uint8Array(
              d.chunk.buffer,
              d.chunk.byteOffset,
              d.chunk.byteLength
            )
          );
          lastWriteSize += d.chunk.byteLength;
        } else {
          throw new ERR_INVALID_ARG_TYPE(
            'chunk',
            ['string', 'ArrayBufferView'],
            d.chunk
          );
        }
      }
      this._unrefTimer();

      this._handle.writer.write(Buffer.concat(buffers)).then(
        () => {
          if (this._handle != null) {
            this._handle.bytesWritten += this[kLastWriteQueueSize] ?? 0;
          } else {
            this[kBytesWritten] += this[kLastWriteQueueSize] ?? 0;
          }
          this[kLastWriteQueueSize] = 0;
          this._unrefTimer();
          cb();
        },
        (err: unknown): void => {
          this[kLastWriteQueueSize] = 0;
          this._unrefTimer();
          cb(err as Error);
        }
      );
    } else {
      let bufferData: Buffer;
      if (typeof data === 'string') {
        bufferData = Buffer.from(data, encoding);
      } else {
        bufferData = data as unknown as Buffer;
      }
      this._handle.writer.write(bufferData).then(
        () => {
          if (this._handle != null) {
            this._handle.bytesWritten += this[kLastWriteQueueSize] ?? 0;
          } else {
            this[kBytesWritten] += this[kLastWriteQueueSize] ?? 0;
          }
          this[kLastWriteQueueSize] = 0;
          this._unrefTimer();
          cb();
        },
        (err: unknown): void => {
          this[kLastWriteQueueSize] = 0;
          this._unrefTimer();

          // Think of the following code:
          //
          // const socket = net.connect(env.SERVER_THAT_DIES_PORT);
          // socket.on('end', () => {
          //   strictEqual(socket.writable, true);
          //   socket.write('hello world');
          //   resolve();
          // });
          //
          // If we don't omit the error message for CLOSED, socket.write()
          // will throw an error. This is not compliant with Node.js behavior.
          if (
            (err as Error).message !== 'This WritableStream has been closed.'
          ) {
            cb(err as Error);
          } else {
            cb();
          }
        }
      );
      lastWriteSize = (data as unknown as Buffer).byteLength;
    }
    this[kLastWriteQueueSize] = lastWriteSize;
  } catch (err) {
    this.destroy(err as Error);
  }
};

Socket.prototype._writev = function (
  this: Socket,
  chunks: SocketWriteData,
  cb: () => void
): void {
  this._writeGeneric(true, chunks, '', cb);
};

Socket.prototype._write = function (
  this: Socket,
  data: SocketWriteData,
  encoding: string,
  cb: (err?: Error) => void
): void {
  this._writeGeneric(false, data, encoding, cb);
};

Socket.prototype._final = function (
  this: Socket,
  cb: (err?: Error) => void
): void {
  if (this.connecting) {
    this.once('connect', () => {
      this._final(cb);
    });
    return;
  }

  // If there is no writer, then there's really nothing left to do here.
  if (this._handle == null) {
    cb();
    return;
  }

  this._handle.writer.close().then(
    (): void => {
      cb();
    },
    (err: unknown): void => {
      cb(err as Error);
    }
  );
};

Socket.prototype.end = function (
  this: Socket,
  // eslint-disable-next-line @typescript-eslint/no-redundant-type-constituents
  data?: string | Uint8Array | NodeJS.BufferEncoding | VoidFunction,
  encoding?: NodeJS.BufferEncoding | VoidFunction,
  cb?: VoidFunction
): Socket {
  // @ts-expect-error this fails after upgrading to @types/node@22.14
  Duplex.prototype.end.call(this, data, encoding, cb);
  return this;
};

// ======================================================================================
// Readable side

Socket.prototype.pause = function (this: Socket): Socket {
  if (this[kBuffer] && !this.connecting && this._handle?.reading) {
    // If the read loop is already running, setting reading to false
    // will interrupt it after the current read completes (if any)
    if (!this.destroyed) {
      this._handle.reading = false;
    }
  }
  return Duplex.prototype.pause.call(this) as unknown as Socket;
};

Socket.prototype.resume = function (this: Socket): Socket {
  if (
    this[kBuffer] &&
    !this.connecting &&
    this._handle &&
    !this._handle.reading
  ) {
    tryReadStart(this);
  }
  return Duplex.prototype.resume.call(this) as unknown as Socket;
};

Socket.prototype.read = function (
  this: Socket,
  n: number
): ReturnType<typeof Duplex.prototype.read> {
  if (
    this[kBuffer] &&
    !this.connecting &&
    this._handle &&
    !this._handle.reading
  ) {
    tryReadStart(this);
  }

  return Duplex.prototype.read.call(this, n);
};

Socket.prototype._read = function (this: Socket, n: number): void {
  if (this.connecting || !this._handle) {
    this.once('connect', () => {
      this._read(n);
    });
  } else if (!this._handle.reading) {
    tryReadStart(this);
  }
};

// ======================================================================================
// Destroy and reset

Socket.prototype._reset = function (this: Socket): Socket {
  this.resetAndClosing = true;
  return this.destroy();
};

Socket.prototype.resetAndDestroy = function (this: Socket): Socket {
  // In Node.js, the resetAndDestroy method is used to "[close] the TCP connection by
  // sending an RST packet and destroy the stream. If this TCP socket is in connecting
  // status, it will send an RST packet and destroy this TCP socket once it is connected.
  // Otherwise, it will call socket.destroy with an ERR_SOCKET_CLOSED Error. If this is
  // not a TCP socket (for example, a pipe), calling this method will immediately throw
  // an ERR_INVALID_HANDLE_TYPE Error." In our implementation we really don't have a way
  // of ensuring whether or not an RST packet is actually sent so this is largely an
  // alias for the existing destroy. If the socket is still connecting, it will be
  // destroyed immediately after the connection is established.
  if (this._handle) {
    if (this.connecting) {
      this.once('connect', () => {
        this._reset();
      });
    } else {
      this._reset();
    }
  } else {
    this.destroy(new ERR_SOCKET_CLOSED());
  }
  return this;
};

Socket.prototype.destroySoon = function (this: Socket): void {
  if (this.writable) {
    this.end();
  }

  if (this.writableFinished) {
    this.destroy();
  } else {
    this.once('finish', () => {
      // Do not call this.destroy.bind(this) since user can override it.
      this.destroy();
    });
  }
};

Socket.prototype._destroy = function (
  this: Socket,
  exception: Error,
  cb: (err?: Error) => void
): void {
  this.connecting = false;

  // eslint-disable-next-line @typescript-eslint/no-this-alias
  for (let s: Socket | null = this; s !== null; s = s._parentWrap) {
    clearTimeout(s[kTimeout] as unknown as number);
  }

  if (this._handle != null) {
    this._handle.socket.close().then(
      () => {
        cleanupAfterDestroy(this, cb, exception);
      },
      (err: unknown) => {
        cleanupAfterDestroy(this, cb, (err || exception) as Error);
      }
    );
  } else {
    cleanupAfterDestroy(this, cb, exception);
  }
};

// ======================================================================================
// Connection

// @ts-expect-error TS2322 Type inconsistencies between types/node
Socket.prototype.connect = function (
  this: Socket,
  ...args: unknown[]
): Socket | undefined {
  let normalized;
  // @ts-expect-error TS7015 Required not to overcomplicate types
  if (Array.isArray(args[0]) && args[0][normalizedArgsSymbol]) {
    normalized = args[0];
  } else {
    normalized = _normalizeArgs(args);
  }
  const options = normalized[0] as TcpSocketConnectOpts & IpcSocketConnectOpts;
  const cb = normalized[1] as ((err: Error | null) => void) | null;

  if (this.connecting) {
    throw new ERR_SOCKET_CONNECTING();
  }
  if (this._aborted) {
    if (cb) {
      cb(new AbortError());
    } else {
      throw new AbortError();
    }
    return undefined;
  }

  if (cb !== null) {
    this.once('connect', cb);
  }

  if (this._parentWrap?.connecting) {
    return this;
  }

  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (options.port === undefined && options.path == null) {
    throw new ERR_MISSING_ARGS(['options', 'port', 'path']);
  }

  if (this.write !== Socket.prototype.write) {
    // eslint-disable-next-line @typescript-eslint/unbound-method
    this.write = Socket.prototype.write;
  }

  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (options.path != null) {
    throw new ERR_INVALID_ARG_VALUE('path', options.path, 'is not supported');
  }

  this[kBytesRead] = 0;
  this[kBytesWritten] = 0;

  // This is required for "reconnection".
  // Previous connected handle needs to emit "close" event.
  // This ensures that the previous handle is closed before initializing a new one.
  if (this._handle) {
    this._handle.socket.close().then(
      () => {
        initializeConnection(this, options);
      },
      () => {
        initializeConnection(this, options);
      }
    );
  } else {
    initializeConnection(this, options);
  }

  return this;
};

Socket.prototype[kReinitializeHandle] = function reinitializeHandle(
  handle: Socket['_handle']
): void {
  this._handle?.socket.close().then(
    () => {
      cleanupAfterDestroy(this);
    },
    (err: unknown) => {
      cleanupAfterDestroy(this, null, err as Error);
    }
  );

  this._handle = handle;
  this._undestroy();
  this._sockname = null;
};

// ======================================================================================
// Socket methods that are not no-ops or nominal impls

Socket.prototype.setNoDelay = function (
  this: Socket,
  _enable?: boolean
): Socket {
  // Ignore this for now.
  // Cloudflare connect() does not support this.
  return this;
};

Socket.prototype.setKeepAlive = function (
  this: Socket,
  _enable?: boolean,
  _initialDelay?: number
): Socket {
  // Ignore this for now.
  // This is used by services like mySQL.
  // TODO(soon): Investigate supporting this.
  return this;
};

// @ts-expect-error TS2322 Intentionally no-op
Socket.prototype.ref = function (this: Socket): void {
  // Intentional no-op
};

// @ts-expect-error TS2322 Intentionally no-op
Socket.prototype.unref = function (this: Socket): void {
  // Intentional no-op
};

Object.defineProperties(Socket.prototype, {
  _connecting: {
    // @ts-expect-error TS2353 Required for __proto__
    __proto__: null,
    get(this: Socket): boolean {
      return this.connecting;
    },
  },
  pending: {
    // @ts-expect-error TS2353 Required for __proto__
    __proto__: null,
    get(this: Socket): boolean {
      return !this._handle || this.connecting;
    },
    configurable: true,
  },
  readyState: {
    // @ts-expect-error TS2353 Required for __proto__
    __proto__: null,
    get(this: Socket): string {
      if (this.connecting) {
        return 'opening';
      } else if (this.readable && this.writable) {
        return 'open';
      } else if (this.readable && !this.writable) {
        return 'readOnly';
      } else if (!this.readable && this.writable) {
        return 'writeOnly';
      }
      return 'closed';
    },
  },
  bufferSize: {
    // @ts-expect-error TS2353 Required for __proto__
    __proto__: null,
    get(this: Socket): number | undefined {
      if (this._handle) {
        return this.writableLength;
      }
      return undefined;
    },
  },
  [kUpdateTimer]: {
    // @ts-expect-error TS2353 Required for __proto__
    __proto__: null,
    get(this: Socket): VoidFunction {
      // eslint-disable-next-line @typescript-eslint/unbound-method
      return this._unrefTimer;
    },
  },
  bytesRead: {
    // @ts-expect-error TS2353 Required for __proto__
    __proto__: null,
    configurable: false,
    enumerable: true,
    get(this: Socket): number {
      return this._handle ? this._handle.bytesRead : this[kBytesRead];
    },
  },
  _bytesDispatched: {
    // @ts-expect-error TS2353 Required for __proto__
    __proto__: null,
    configurable: false,
    enumerable: true,
    get(this: Socket): number {
      return this._handle ? this._handle.bytesWritten : this[kBytesWritten];
    },
  },
  bytesWritten: {
    // @ts-expect-error TS2353 Required for __proto__
    __proto__: null,
    configurable: false,
    enumerable: true,
    get(this: Socket): number | undefined {
      let bytes = this._bytesDispatched;
      const data = this._pendingData as unknown as Buffer | string | null;
      const encoding = this._pendingEncoding;
      const writableBuffer = this.writableBuffer;

      if (!writableBuffer) return undefined;

      if (Array.isArray(data)) {
        // Was a writev, iterate over chunks to get total length
        for (let i = 0; i < data.length; i++) {
          const chunk = data[i] as Buffer | null;

          if (chunk == null) {
            continue;
          }

          // @ts-expect-error TS2339 allBuffers doesn't exist on type.
          if (chunk instanceof Buffer || data.allBuffers) bytes += chunk.length;
          else {
            bytes += Buffer.byteLength(
              // @ts-expect-error TS2339 TODO(soon): Use correct type here.
              chunk.chunk as Buffer,
              // @ts-expect-error TS2339 TODO(soon): Use correct type here.
              chunk.encoding as string
            );
          }
        }
      } else if (data) {
        // Writes are either a string or a Buffer.
        if (typeof data !== 'string') {
          bytes += data.length;
        } else {
          bytes += Buffer.byteLength(data, encoding);
        }
      } else {
        const flushed =
          this._handle != null
            ? this._handle.bytesWritten
            : this[kBytesWritten];
        return this.writableLength + flushed;
      }

      return bytes;
    },
  },
  remoteAddress: {
    // @ts-expect-error TS2353 Required for __proto__
    __proto__: null,
    configurable: false,
    enumerable: true,
    get(this: Socket): unknown {
      return this._getpeername().address;
    },
  },
  remoteFamily: {
    // @ts-expect-error TS2353 Required for __proto__
    __proto__: null,
    configurable: false,
    enumerable: true,
    get(this: Socket): unknown {
      return this._getpeername().family;
    },
  },
  remotePort: {
    // @ts-expect-error TS2353 Required for __proto__
    __proto__: null,
    configurable: false,
    enumerable: true,
    get(this: Socket): unknown {
      return this._getpeername().port;
    },
  },
  localAddress: {
    // @ts-expect-error TS2353 Required for __proto__
    __proto__: null,
    configurable: false,
    enumerable: true,
    get(this: Socket): string | undefined {
      return this._getsockname().address;
    },
  },
  localPort: {
    // @ts-expect-error TS2353 Required for __proto__
    __proto__: null,
    configurable: false,
    enumerable: true,
    get(this: Socket): number | undefined {
      return this._getsockname().port;
    },
  },
  localFamily: {
    // @ts-expect-error TS2353 Required for __proto__
    __proto__: null,
    configurable: false,
    enumerable: true,
    get(this: Socket): string | undefined {
      return this._getsockname().family;
    },
  },
});

// ======================================================================================
// Helper/utility methods

function cleanupAfterDestroy(
  socket: Socket,
  cb?: ((err?: Error) => void) | null,
  error?: Error
): void {
  const isException = error != null;
  if (socket._handle != null) {
    socket[kBytesRead] = socket._handle.bytesRead;
    socket[kBytesWritten] = socket._handle.bytesWritten;
    socket.resetAndClosing = false;
  }
  socket[kLastWriteQueueSize] = 0;
  socket[kSocketInfo] = null;

  // If there's an error, emit it before the close event
  if (error != null) {
    socket.emit('error', error);
  }

  cb?.(error);
  socket.emit('close', isException);
}

function initializeConnection(
  socket: Socket,
  options: TcpSocketConnectOpts
): void {
  const {
    host = 'localhost',
    family,
    hints,
    autoSelectFamily,
    autoSelectFamilyAttemptTimeout,
    lookup,
    localAddress,
    localPort,
  } = options;
  let { port = 0 } = options;
  if (localAddress && !isIP(localAddress)) {
    throw new ERR_INVALID_IP_ADDRESS(localAddress);
  }

  if (localPort) {
    validateNumber(localPort, 'options.localPort');
  }

  if (typeof port !== 'number' && typeof port !== 'string') {
    throw new ERR_INVALID_ARG_TYPE('options.port', ['number', 'string'], port);
  }
  validatePort(port);
  port |= 0;

  if (autoSelectFamily != null) {
    // We don't support this option.
    // We shouldn't throw this because services like mongodb depends on it.
    // TODO(soon): Investigate supporting this.
    validateBoolean(autoSelectFamily, 'options.autoSelectFamily');
  }

  if (autoSelectFamilyAttemptTimeout != null) {
    // We don't support this option.
    // We shouldn't throw this because services like mongodb depends on it.
    // TODO(soon): Investigate supporting this.
    validateInt32(
      autoSelectFamilyAttemptTimeout,
      'options.autoSelectFamilyAttemptTimeout',
      1
    );
  }

  socket._unrefTimer();
  socket.connecting = true;

  const continueConnection = (
    host: unknown,
    port: number,
    family: number | string
  ): void => {
    socket[kSocketInfo] = {
      remoteAddress: {
        address: host,
        port,
        family: family === 4 ? 'IPv4' : family === 6 ? 'IPv6' : undefined,
      },
    };

    if (family === 6) {
      // The host is an IPv6 address. We need to wrap it in square brackets.
      host = `[${host}]`;
    }

    // @ts-expect-error TS2540 Unnecessary error due to using @types/node
    socket.autoSelectFamilyAttemptedAddresses = [`${host}:${port}`];

    socket.emit('connectionAttempt', host, port, addressType);

    socket._host = `${host}`;

    try {
      const handle = inner.connect(`${host}:${port}`, {
        allowHalfOpen: socket.allowHalfOpen,
        // A Node.js socket is always capable of being upgraded to the TLS socket.
        secureTransport: socket.encrypted ? 'on' : 'starttls',
        // We are not going to pass the high water-mark here. The outer Node.js
        // stream will implement the appropriate backpressure for us.
      });

      // Our version of the socket._handle is necessarily different from Node.js'.
      // It serves the same purpose but any code that may exist that is depending
      // on `_handle` being a particular type (which it shouldn't be) will fail.
      socket._handle = {
        socket: handle,
        writer: handle.writable.getWriter(),
        reader: handle.readable.getReader({ mode: 'byob' }),
        bytesRead: 0,
        bytesWritten: 0,
        reading: false,
        options: {
          host: socket._host,
          port,
          addressType,
        },
      };

      // We need to undestroy the stream to connect to it.
      socket._undestroy();
      socket._sockname = null;

      handle.opened.then(onConnectionOpened.bind(socket), (err: unknown) => {
        socket.emit('connectionAttemptFailed', host, port, addressType, err);
        socket.destroy(err as Error);
      });

      handle.closed.then(
        onConnectionClosed.bind(socket),
        (error: unknown): void => {
          // Do not call socket.destroy.bind(socket) since user can override it.
          socket.destroy(error as Error);
        }
      );
    } catch (err) {
      socket.destroy(err as Error);
    }
  };

  if (lookup != null) {
    validateFunction(lookup, 'options.lookup');
  }

  const addressType = isIP(host);

  // The host is not an IP address. That's allowed in our implementation, but let's
  // see if the user provided a lookup function. If not, we'll skip.
  if (addressType === 0 && lookup != null) {
    // Looks like we have a lookup function! Let's call it. The expectation is that
    // the lookup function will produce a good IP address from the non-IP address
    // that is given. How that is done is left entirely up to the application code.
    // The connection attempt will continue once the lookup function invokes the
    // given callback.
    const lookupOptions = { family: family || addressType, hints };
    lookup(
      host,
      lookupOptions,
      (err: Error | null, address: string, family: number | string): void => {
        socket.emit('lookup', err, address, family, host);
        if (err) {
          socket.destroy(err);
          return;
        }
        if (isIP(address) === 0) {
          throw new ERR_INVALID_IP_ADDRESS(address);
        }
        if (
          family !== 4 &&
          family !== 6 &&
          family !== 'IPv4' &&
          family !== 'IPv6'
        ) {
          throw new ERR_INVALID_ARG_VALUE('family', family, 'must be 4 or 6');
        }
        continueConnection(address, port, family);
      }
    );
  } else {
    continueConnection(host, port, addressType);
  }
}

export function onConnectionOpened(this: Socket): void {
  // Callback may come after call to destroy
  if (this.destroyed) {
    return;
  }

  this.connecting = false;
  this._sockname = null;
  this._unrefTimer();
  this.emit('connect');
  this.emit('ready');
  if (!this.isPaused()) {
    tryReadStart(this);
  }
}

export function onConnectionClosed(this: Socket): void {
  if (this._handle?.socket.upgraded) {
    // The socket is being upgraded from insecure to TLS.
    // No need to handle this particular close event.
    return;
  }
  // eslint-disable-next-line @typescript-eslint/no-this-alias
  for (let s: Socket | null = this; s !== null; s = s._parentWrap) {
    clearTimeout(s[kTimeout] as unknown as number);
  }

  if (!this.destroyed) {
    // We have to manually trigger an 'end' event because we are using
    // BYOB buffers with Socket class.
    this.emit('end');
  }
}

async function startRead(socket: Socket): Promise<void> {
  if (!socket._handle) return;
  const reader = socket._handle.reader;
  try {
    while (socket._handle.reading === true) {
      const generatedBuffer = socket[kBufferGen]?.();

      // Let's be extra cautious here and handle nullish values.
      if (generatedBuffer == null || generatedBuffer.length === 0) {
        // When reading a static buffer with fixed length, it's highly likely to
        // read the whole buffer in a single take, which will make the second
        // operation to read an empty buffer.
        //
        // Workerd throws the following exception when reading empty buffers
        // TypeError: You must call read() on a "byob" reader with a positive-sized TypedArray object.
        // Therefore, let's skip calling read operation and stop reading here.
        break;
      }

      // The [kBufferGen] function should always be a function that returns
      // a Uint8Array we can read into.
      const { value, done } = await reader.read(generatedBuffer);

      // Make sure the socket was not destroyed while we were waiting.
      // If it was, we're going to throw away the chunk of data we just
      // read.
      if (socket.destroyed) {
        // Doh! Well, this is awkward. Let's just stop reading and return.
        // There's really nothing else we should try to do here.
        break;
      }

      // Reset the timeout timer since we received data.
      socket._unrefTimer();

      if (done) {
        // All done! If allowHalfOpen is true, then this will just end the
        // readable side of the socket. If allowHalfOpen is false, then this
        // should allow the current write queue to drain but not allow any
        // further writes to be queued.
        socket.push(null);
        break;
      }

      // If the byteLength is zero, skip the push.
      if (value.byteLength === 0) {
        continue;
      }
      socket._handle.bytesRead += value.byteLength;

      // The socket API is expected to produce Buffer instances, not Uint8Arrays
      const buffer = Buffer.from(
        value.buffer,
        value.byteOffset,
        value.byteLength
      );

      if (typeof socket[kBufferCb] === 'function') {
        if (socket[kBufferCb](buffer.byteLength, buffer) === false) {
          // If the callback returns explicitly false (not falsy) then
          // we're being asked to stop reading for now.
          break;
        }
        continue;
      }

      // Because we're pushing the buffer onto the stream we can't use the shared
      // buffer here or the next read will overwrite it! We need to copy. For the
      // more efficient version, use onread.
      if (!socket.push(Buffer.from(buffer))) {
        // If push returns false, we've hit the high water mark and should stop
        // reading until the stream requests to start reading again.
        break;
      }
    }
  } catch (_err) {
    // Ignore error, and don't log them.
    // This is mostly triggered for invalid sockets with following errors:
    // - "This ReadableStream belongs to an object that is closing."
  } finally {
    // Disable eslint to match Node.js behavior
    // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
    if (socket._handle != null) {
      socket._handle.reading = false;
    }
  }
}

export function tryReadStart(socket: Socket): void {
  if (socket._handle != null) {
    socket._handle.reading = true;
  }
  startRead(socket).catch((err: unknown) => socket.destroy(err as Error));
}

function writeAfterFIN(
  this: Socket,
  chunk: Uint8Array | string,
  encoding?: NodeJS.BufferEncoding | null | ((err?: Error) => void),
  cb?: (err?: Error) => void
): boolean {
  if (!this.writableEnded) {
    // @ts-expect-error TS2554 Required due to @types/node
    return Duplex.prototype.write.call(this, chunk, encoding, cb);
  }

  if (typeof encoding === 'function') {
    cb = encoding;
    encoding = null;
  }

  const er = new EPIPE();

  if (cb != null && typeof cb === 'function') {
    queueMicrotask(() => {
      cb(er);
    });
  }
  this.destroy(er);

  return false;
}

function onReadableStreamEnd(this: Socket): void {
  if (!this.allowHalfOpen) {
    this.write = writeAfterFIN;
  }
}

export function getTimerDuration(msecs: unknown, name: string): number {
  validateNumber(msecs, name);
  if (msecs < 0 || !Number.isFinite(msecs)) {
    throw new ERR_OUT_OF_RANGE(name, 'a non-negative finite number', msecs);
  }

  // Ensure that msecs fits into signed int32
  if (msecs > TIMEOUT_MAX) {
    return TIMEOUT_MAX;
  }

  return msecs;
}

export function toNumber(x: unknown): number | false {
  return (x = Number(x)) >= 0 ? (x as number) : false;
}

export function isPipeName(s: unknown): boolean {
  return typeof s === 'string' && toNumber(s) === false;
}

export type NormalizedArgs = [
  {
    path?: string;
    port?: number;
    host?: string;
  },
  ((...args: unknown[]) => void) | null,
];

export function _normalizeArgs(args: unknown[]): NormalizedArgs {
  let arr: NormalizedArgs;

  if (args.length === 0) {
    arr = [{}, null];
    // @ts-expect-error TS2554 Required due to @types/node
    arr[normalizedArgsSymbol] = true;
    return arr;
  }

  const arg0 = args[0];
  let options: {
    path?: string;
    port?: number;
    host?: string;
  } = {};
  if (typeof arg0 === 'object' && arg0 !== null) {
    // (options[...][, cb])
    options = arg0;
  } else if (isPipeName(arg0)) {
    // (path[...][, cb])
    options.path = arg0 as string;
  } else {
    // ([port][, host][...][, cb])
    options.port = arg0 as number;
    if (args.length > 1 && typeof args[1] === 'string') {
      // eslint-disable-next-line @typescript-eslint/no-unnecessary-type-assertion
      options.host = args[1] as string;
    }
  }

  const cb = args[args.length - 1];
  if (typeof cb !== 'function') arr = [options, null];
  else arr = [options, cb as (...args: unknown[]) => unknown];

  // @ts-expect-error TS2554 Required due to @types/node
  arr[normalizedArgsSymbol] = true;
  return arr;
}

function addClientAbortSignalOption(self: Socket, signal: AbortSignal): void {
  validateAbortSignal(signal, 'options.signal');
  let disposable: Disposable | undefined;

  function onAbort(): void {
    disposable?.[Symbol.dispose]();
    self._aborted = true;
  }

  if (signal.aborted) {
    queueMicrotask(onAbort);
  } else {
    queueMicrotask(() => {
      disposable = addAbortListener(signal, onAbort);
    });
  }
}

function addAbortListener(
  signal: AbortSignal | undefined,
  listener: VoidFunction
): Disposable {
  if (signal === undefined) {
    throw new ERR_INVALID_ARG_TYPE('signal', 'AbortSignal', signal);
  }
  validateAbortSignal(signal, 'signal');
  validateFunction(listener, 'listener');

  let removeEventListener: undefined | (() => void);
  if (signal.aborted) {
    queueMicrotask(() => {
      listener();
    });
  } else {
    signal.addEventListener('abort', listener, { once: true });
    removeEventListener = (): void => {
      signal.removeEventListener('abort', listener);
    };
  }
  return {
    // @ts-expect-error TS2353 Required for __proto__
    __proto__: null,
    [Symbol.dispose](): void {
      removeEventListener?.();
    },
  };
}

// ======================================================================================
// The rest of the exports

export function connect(...args: unknown[]): Socket {
  const normalized = _normalizeArgs(args);
  const options = normalized[0] as SocketOptions;
  const socket: Socket = new Socket(options);
  if (options.timeout) {
    socket.setTimeout(options.timeout);
  }
  if (socket.destroyed) {
    return socket;
  }
  return socket.connect(normalized as unknown as SocketConnectOpts);
}

export const createConnection = connect;

export function createServer(): void {
  throw new Error('createServer() is not implemented');
}

export function getDefaultAutoSelectFamily(): boolean {
  // This is the only value we support.
  return false;
}

export function setDefaultAutoSelectFamily(val: unknown): void {
  if (!val) return;
  throw new ERR_INVALID_ARG_VALUE('val', val);
}

// We don't actually make use of this. It's here only for compatibility.
// The value is not used anywhere.
let autoSelectFamilyAttemptTimeout: number = 10;

export function getDefaultAutoSelectFamilyAttemptTimeout(): number {
  return autoSelectFamilyAttemptTimeout;
}

export function setDefaultAutoSelectFamilyAttemptTimeout(val: unknown): void {
  validateInt32(val, 'val', 1);
  if (val < 10) val = 10;
  autoSelectFamilyAttemptTimeout = val as number;
}

export function isIP(input: unknown): number {
  if (isIPv4(input)) return 4;
  if (isIPv6(input)) return 6;
  return 0;
}

export function isIPv4(input: unknown): boolean {
  input = typeof input !== 'string' ? `${input}` : input;
  return IPv4Reg.test(input as string);
}

export function isIPv6(input: unknown): boolean {
  input = typeof input !== 'string' ? `${input}` : input;
  return IPv6Reg.test(input as string);
}

// ======================================================================================

export class SocketAddress implements _SocketAddress {
  #address: string;
  #port: number;
  #family: IPVersion;
  #flowlabel: number;

  static isSocketAddress(value: unknown): value is SocketAddress {
    return value instanceof SocketAddress;
  }

  constructor(options: SocketAddressInitOptions = {}) {
    validateObject(options, 'options');
    this.#family = options.family || 'ipv4';
    // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
    if (typeof this.#family?.toLowerCase === 'function')
      this.#family = this.#family.toLowerCase() as IPVersion;
    validateOneOf(this.#family, 'options.family', ['ipv4', 'ipv6']);

    const {
      address = this.#family === 'ipv4' ? '127.0.0.1' : '::',
      port = 0,
      flowlabel = 0,
    } = options;

    validateString(address, 'options.address');
    const _port = validatePort(port, 'options.port');
    validateUint32(flowlabel, 'options.flowlabel', false);

    switch (this.#family) {
      case 'ipv4':
        if (!isIPv4(address)) {
          throw new ERR_INVALID_ADDRESS();
        }
        break;
      case 'ipv6':
        if (!isIPv6(address)) {
          throw new ERR_INVALID_ADDRESS();
        }
        break;
    }

    // Node.js' implementation is a bit more complicated since it is backed
    // by a C++ class wrapping an actual socket address structure. We don't
    // need that here, so we keep things simple.

    this.#address = address;
    this.#port = _port;
    this.#flowlabel = flowlabel;
  }

  get address(): string {
    return this.#address;
  }

  get port(): number {
    return this.#port;
  }

  get family(): IPVersion {
    return this.#family;
  }

  get flowlabel(): number {
    return this.#flowlabel;
  }

  [kInspect](depth: number, options: InspectOptions): string | this {
    if (depth < 0) return this;

    const opts: InspectOptions = {
      ...options,
      depth: options.depth == null ? null : options.depth - 1,
    };

    // @ts-expect-error TS2769 not all the overloads are compatible
    return `SocketAddress ${inspect(this.toJSON(), opts)}`;
  }

  toJSON(): {
    address: string;
    port: number;
    family: IPVersion;
    flowlabel: number;
  } {
    return {
      address: this.address,
      port: this.port,
      family: this.family,
      flowlabel: this.flowlabel,
    };
  }

  static parse(input: string): SocketAddress | undefined {
    validateString(input, 'input');
    try {
      const parsed = URL.parse(`http://${input}`);
      if (parsed == null) {
        return undefined;
      }
      const { hostname: address, port } = parsed;
      if (address.startsWith('[') && address.endsWith(']')) {
        return new SocketAddress({
          address: address.slice(1, -1),
          // @ts-expect-error TS2362 port will be a string, this converts it
          port: port | 0,
          family: 'ipv6',
        });
      }
      // @ts-expect-error TS2362 port will be a string, this converts it
      return new SocketAddress({ address, port: port | 0 });
    } catch {
      // Ignore errors here. Return undefined if the input cannot
      // be successfully parsed or is not a proper socket address.
    }
    return undefined;
  }
}

// ======================================================================================

// Note: the bulk of the following BlockList related code was authored by claude...
// The implementation in Node.js is split between javascript and C++.
// Here we do the entire implementation as TypeScript simply because
// we don't need the C++ parts at all.

const kIpv6Regex = /^::ffff:(\d+\.\d+\.\d+\.\d+)$/i;
const kIpv6RangeRegex =
  /Range: IPv6 ([0-9a-fA-F:]{1,39})-([0-9a-fA-F:]{1,39})/i;
const kIpv6AddressRegex = /Address: IPv6 ([0-9a-fA-F:]{1,39})/i;
const kIpv6SubnetRegex = /Subnet: IPv6 ([0-9a-fA-F:]{1,39})\/(\d{1,3})/i;
const kIpv4RangeRegex =
  /Range: IPv4 (\d{1,3}(?:\.\d{1,3}){3})-(\d{1,3}(?:\.\d{1,3}){3})/;
const kIpv4AddressRegex = /Address: IPv4 (\d{1,3}(?:\.\d{1,3}){3})/;
const kIpv4SubnetRegex = /Subnet: IPv4 (\d{1,3}(?:\.\d{1,3}){3})\/(\d{1,2})/;

// IPv4/IPv6 CIDR network utilities
function parseIPv4(ip: string): FixedLengthArray<number, 4> | undefined {
  // When the input is not a valid IPv4 address, return an empty array.
  const parts = ip.split('.');
  if (parts.length !== 4) return undefined;

  const nums = [0, 0, 0, 0] as FixedLengthArray<number, 4>;

  for (let n = 0; n < parts.length; n++) {
    const part = parts[n];
    if (part === undefined) return undefined;
    const num = parseInt(part, 10);
    if (isNaN(num) || num < 0 || num > 255) return undefined;
    nums[n] = num;
  }

  return nums;
}

function parseIPv6(ip: string): FixedLengthArray<number, 8> | undefined {
  // Handle IPv4-mapped IPv6 addresses
  if (ip.includes('.')) {
    const match = ip.match(kIpv6Regex);
    if (match != null && match[1]) {
      const ipv4Parts = parseIPv4(match[1]);
      if (ipv4Parts !== undefined) {
        return [
          0,
          0,
          0,
          0,
          0,
          0xffff,
          (ipv4Parts[0] << 8) | ipv4Parts[1],
          (ipv4Parts[2] << 8) | ipv4Parts[3],
        ];
      }
    }
  }

  // Expand :: notation
  let expanded = ip;
  if (ip.includes('::')) {
    const parts = ip.split('::');
    if (parts.length === 2) {
      const left = parts[0]?.split(':') ?? [];
      const right = parts[1]?.split(':') ?? [];
      const missing = 8 - left.length - right.length;
      const middle = Array.from({ length: missing }).fill('0');
      expanded = [...left, ...middle, ...right].join(':');
    }
  }

  const parts = expanded.split(':');
  if (parts.length !== 8) return undefined;

  const nums = parts.map((p) => {
    const num = parseInt(p || '0', 16);
    return num >= 0 && num <= 0xffff ? num : -1;
  }) as FixedLengthArray<number, 8>;

  return nums.includes(-1) ? undefined : nums;
}

function ipv4ToNumber(ip: string): number {
  const parts = parseIPv4(ip);
  if (parts === undefined)
    throw new ERR_INVALID_IP_ADDRESS('Invalid IPv4 address');
  return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

function ipv6ToBigInt(ip: string): bigint {
  const parts = parseIPv6(ip);
  if (parts === undefined)
    throw new ERR_INVALID_IP_ADDRESS('Invalid IPv6 address');

  let result = 0n;
  for (let i = 0; i < 8; i++) {
    const part = parts[i];
    result = (result << 16n) | BigInt(part as number);
  }
  return result;
}

function isInIPv4Subnet(ip: string, network: string, prefix: number): boolean {
  if (prefix < 0 || prefix > 32) return false;
  if (prefix === 0) return true;

  const ipNum = ipv4ToNumber(ip);
  const netNum = ipv4ToNumber(network);
  // Create a subnet mask by shifting all 1s left by (32 - prefix) bits
  // This creates a mask with 'prefix' number of leading 1s followed by 0s
  // For example, prefix=24 creates mask 0xffffff00 (255.255.255.0)
  const mask = (0xffffffff << (32 - prefix)) >>> 0;

  return (ipNum & mask) === (netNum & mask);
}

function isInIPv6Subnet(ip: string, network: string, prefix: number): boolean {
  if (prefix < 0 || prefix > 128) return false;
  if (prefix === 0) return true;

  const ipBig = ipv6ToBigInt(ip);
  const netBig = ipv6ToBigInt(network);

  // Create a 128-bit subnet mask for IPv6 by shifting all 1s left by (128 - prefix) bits
  // This creates a mask with 'prefix' number of leading 1s followed by 0s
  // The AND with 0xfff...f ensures we stay within 128 bits
  // For example, prefix=64 creates a mask with 64 leading 1s and 64 trailing 0s
  const mask =
    (0xffffffffffffffffffffffffffffffffn << BigInt(128 - prefix)) &
    0xffffffffffffffffffffffffffffffffn;

  return (ipBig & mask) === (netBig & mask);
}

function compareIPv4(a: string, b: string): number {
  const aNum = ipv4ToNumber(a);
  const bNum = ipv4ToNumber(b);
  return aNum - bNum;
}

function compareIPv6(a: string, b: string): number {
  const aBig = ipv6ToBigInt(a);
  const bBig = ipv6ToBigInt(b);
  return aBig < bBig ? -1 : aBig > bBig ? 1 : 0;
}

function formatIPFamily(family: 'ipv4' | 'ipv6'): 'IPv4' | 'IPv6' {
  return family === 'ipv4' ? 'IPv4' : 'IPv6';
}

interface BlockListRule {
  type: 'address' | 'range' | 'subnet';
  family: 'ipv4' | 'ipv6';
  toString(): string;
  check(address: string, family: 'ipv4' | 'ipv6'): boolean;
}

class AddressRule implements BlockListRule {
  type = 'address' as const;
  address: string;
  family: 'ipv4' | 'ipv6';

  constructor(address: string, family: 'ipv4' | 'ipv6') {
    this.address = address;
    this.family = family;
  }

  toString(): string {
    return `Address: ${formatIPFamily(this.family)} ${this.address}`;
  }

  check(address: string, family: 'ipv4' | 'ipv6'): boolean {
    if (this.family !== family) {
      // Handle IPv4-mapped IPv6 addresses
      if (
        this.family === 'ipv4' &&
        family === 'ipv6' &&
        address.startsWith('::ffff:')
      ) {
        const ipv4Part = address.substring(7);
        return this.address === ipv4Part;
      }
      return false;
    }
    return this.address === address;
  }
}

class RangeRule implements BlockListRule {
  type = 'range' as const;
  start: string;
  end: string;
  family: 'ipv4' | 'ipv6';

  constructor(start: string, end: string, family: 'ipv4' | 'ipv6') {
    this.start = start;
    this.end = end;
    this.family = family;
  }

  toString(): string {
    return `Range: ${formatIPFamily(this.family)} ${this.start}-${this.end}`;
  }

  check(address: string, family: 'ipv4' | 'ipv6'): boolean {
    if (this.family !== family) {
      // Handle IPv4-mapped IPv6 for IPv4 ranges
      if (
        this.family === 'ipv4' &&
        family === 'ipv6' &&
        address.startsWith('::ffff:')
      ) {
        const ipv4Part = address.substring(7);
        const cmpStart = compareIPv4(ipv4Part, this.start);
        const cmpEnd = compareIPv4(ipv4Part, this.end);
        return cmpStart >= 0 && cmpEnd <= 0;
      }
      return false;
    }

    if (family === 'ipv4') {
      const cmpStart = compareIPv4(address, this.start);
      const cmpEnd = compareIPv4(address, this.end);
      return cmpStart >= 0 && cmpEnd <= 0;
    } else {
      const cmpStart = compareIPv6(address, this.start);
      const cmpEnd = compareIPv6(address, this.end);
      return cmpStart >= 0 && cmpEnd <= 0;
    }
  }
}

class SubnetRule implements BlockListRule {
  type = 'subnet' as const;
  network: string;
  prefix: number;
  family: 'ipv4' | 'ipv6';

  constructor(network: string, prefix: number, family: 'ipv4' | 'ipv6') {
    this.network = network;
    this.prefix = prefix;
    this.family = family;
  }

  toString(): string {
    return `Subnet: ${formatIPFamily(this.family)} ${this.network}/${this.prefix}`;
  }

  check(address: string, family: 'ipv4' | 'ipv6'): boolean {
    if (this.family !== family) {
      // Handle IPv4-mapped IPv6 for IPv4 subnets
      if (
        this.family === 'ipv4' &&
        family === 'ipv6' &&
        address.startsWith('::ffff:')
      ) {
        const ipv4Part = address.substring(7);
        return isInIPv4Subnet(ipv4Part, this.network, this.prefix);
      }
      return false;
    }

    if (family === 'ipv4') {
      return isInIPv4Subnet(address, this.network, this.prefix);
    } else {
      return isInIPv6Subnet(address, this.network, this.prefix);
    }
  }
}

export class BlockList {
  #rules: BlockListRule[] = [];

  static isBlockList(value: unknown): value is BlockList {
    return value instanceof BlockList;
  }

  addAddress(
    address: string | SocketAddress,
    family: 'ipv4' | 'ipv6' = 'ipv4'
  ): void {
    if (SocketAddress.isSocketAddress(address)) {
      this.#rules.push(new AddressRule(address.address, address.family));
    } else {
      validateString(address, 'address');
      validateOneOf(family, 'family', ['ipv4', 'ipv6']);

      // Validate the address format
      if (family === 'ipv4' && !isIPv4(address)) {
        throw new ERR_INVALID_IP_ADDRESS('Invalid IPv4 address');
      }
      if (family === 'ipv6' && !isIPv6(address)) {
        throw new ERR_INVALID_IP_ADDRESS('Invalid IPv6 address');
      }

      this.#rules.push(new AddressRule(address, family));
    }
  }

  addRange(
    start: string | SocketAddress,
    end: string | SocketAddress,
    family: 'ipv4' | 'ipv6' = 'ipv4'
  ): void {
    let startAddr: string;
    let endAddr: string;
    let addrFamily: 'ipv4' | 'ipv6';

    if (SocketAddress.isSocketAddress(start)) {
      startAddr = start.address;
      addrFamily = start.family;
    } else {
      validateString(start, 'start');
      validateOneOf(family, 'family', ['ipv4', 'ipv6']);
      startAddr = start;
      addrFamily = family;
    }

    if (SocketAddress.isSocketAddress(end)) {
      endAddr = end.address;
      if (SocketAddress.isSocketAddress(start) && end.family !== addrFamily) {
        throw new ERR_INVALID_ARG_VALUE(
          'end',
          end,
          'must be same family as start'
        );
      }
    } else {
      validateString(end, 'end');
      endAddr = end;
    }

    // Validate addresses
    if (addrFamily === 'ipv4') {
      if (!isIPv4(startAddr) || !isIPv4(endAddr)) {
        throw new ERR_INVALID_IP_ADDRESS('Invalid IPv4 address');
      }
      if (compareIPv4(startAddr, endAddr) > 0) {
        throw new ERR_INVALID_ARG_VALUE(
          'start',
          startAddr,
          'must come before end'
        );
      }
    } else {
      if (!isIPv6(startAddr) || !isIPv6(endAddr)) {
        throw new ERR_INVALID_IP_ADDRESS('Invalid IPv6 address');
      }
      if (compareIPv6(startAddr, endAddr) > 0) {
        throw new ERR_INVALID_ARG_VALUE(
          'start',
          startAddr,
          'must come before end'
        );
      }
    }

    this.#rules.push(new RangeRule(startAddr, endAddr, addrFamily));
  }

  addSubnet(
    network: string | SocketAddress,
    prefix: number,
    family: 'ipv4' | 'ipv6' = 'ipv4'
  ): void {
    let networkAddr: string;
    let addrFamily: 'ipv4' | 'ipv6';

    if (SocketAddress.isSocketAddress(network)) {
      networkAddr = network.address;
      addrFamily = network.family;
    } else {
      validateString(network, 'network');
      validateOneOf(family, 'family', ['ipv4', 'ipv6']);
      networkAddr = network;
      addrFamily = family;
    }

    validateInt32(prefix, 'prefix');

    // Validate prefix range
    if (addrFamily === 'ipv4') {
      if (prefix < 0 || prefix > 32) {
        throw new ERR_OUT_OF_RANGE('prefix', 'between 0 and 32', prefix);
      }
      if (!isIPv4(networkAddr)) {
        throw new ERR_INVALID_IP_ADDRESS('Invalid IPv4 address');
      }
    } else {
      if (prefix < 0 || prefix > 128) {
        throw new ERR_OUT_OF_RANGE('prefix', 'between 0 and 128', prefix);
      }
      if (!isIPv6(networkAddr)) {
        throw new ERR_INVALID_IP_ADDRESS('Invalid IPv6 address');
      }
    }

    this.#rules.push(new SubnetRule(networkAddr, prefix, addrFamily));
  }

  check(
    address: string | SocketAddress,
    family: 'ipv4' | 'ipv6' = 'ipv4'
  ): boolean {
    let checkAddr: string;
    let checkFamily: 'ipv4' | 'ipv6';

    if (SocketAddress.isSocketAddress(address)) {
      checkAddr = address.address;
      checkFamily = address.family;
    } else {
      validateString(address, 'address');
      validateOneOf(family, 'family', ['ipv4', 'ipv6']);
      checkAddr = address;
      checkFamily = family.toLowerCase() as 'ipv4' | 'ipv6';
    }

    // Validate address format
    try {
      if (checkFamily === 'ipv4' && !isIPv4(checkAddr)) {
        return false;
      }
      if (checkFamily === 'ipv6' && !isIPv6(checkAddr)) {
        return false;
      }
    } catch {
      return false;
    }

    // Check against all rules
    for (const rule of this.#rules) {
      if (rule.check(checkAddr, checkFamily)) {
        return true;
      }
    }

    return false;
  }

  get rules(): string[] {
    return this.#rules.map((rule) => rule.toString());
  }

  [kInspect](depth: number, options: InspectOptions): string {
    if (depth < 0) return '[BlockList]';

    const opts: InspectOptions = {
      ...options,
      depth: options.depth == null ? null : options.depth - 1,
    };

    // @ts-expect-error TS2769 not all the overloads are compatible
    return `BlockList {\n  rules: ${inspect(this.rules, opts)}\n}`;
  }

  toJSON(): string[] {
    return this.rules;
  }

  fromJSON(data: string | string[]): void {
    let rules: string[];

    if (typeof data === 'string') {
      try {
        rules = JSON.parse(data) as string[];
      } catch {
        throw new ERR_INVALID_ARG_VALUE('data', data, 'must be valid JSON');
      }
    } else {
      rules = data;
    }

    validateArray(rules, 'data');

    for (const item of data) {
      if (item.includes('IPv4')) {
        // IPv4 subnet pattern
        const subnetMatch = item.match(kIpv4SubnetRegex);
        if (subnetMatch) {
          const [, network, prefix] = subnetMatch;
          if (network === undefined || prefix === undefined) {
            // Skip the rule if parsing failed.
            continue;
          }
          this.addSubnet(network, parseInt(prefix, 10));
          continue;
        }

        // IPv4 address pattern
        const addressMatch = item.match(kIpv4AddressRegex);

        if (addressMatch) {
          const [, address] = addressMatch;
          if (address === undefined) {
            // Skip the rule if parsing failed.
            continue;
          }
          this.addAddress(address);
          continue;
        }

        // IPv4 range pattern
        const rangeMatch = item.match(kIpv4RangeRegex);
        if (rangeMatch) {
          const [, start, end] = rangeMatch;
          if (start === undefined || end === undefined) {
            // Skip the rule if parsing failed.
            continue;
          }
          this.addRange(start, end);
          continue;
        }
      }

      if (item.includes('IPv6')) {
        // IPv6 subnet pattern
        const ipv6SubnetMatch = item.match(kIpv6SubnetRegex);
        if (ipv6SubnetMatch) {
          const [, network, prefix] = ipv6SubnetMatch;
          if (network === undefined || prefix === undefined) {
            // Skip the rule if parsing failed.
            continue;
          }
          this.addSubnet(network, parseInt(prefix, 10), 'ipv6');
          continue;
        }

        // IPv6 address pattern
        const ipv6AddressMatch = item.match(kIpv6AddressRegex);
        if (ipv6AddressMatch) {
          const [, address] = ipv6AddressMatch;
          if (address === undefined) {
            // Skip the rule if parsing failed.
            continue;
          }
          this.addAddress(address, 'ipv6');
          continue;
        }

        // IPv6 range pattern
        const ipv6RangeMatch = item.match(kIpv6RangeRegex);
        if (ipv6RangeMatch) {
          const [, start, end] = ipv6RangeMatch;
          if (start === undefined || end === undefined) {
            // Skip the rule if parsing failed.
            continue;
          }
          this.addRange(start, end, 'ipv6');
          continue;
        }
      }
    }
  }
}
