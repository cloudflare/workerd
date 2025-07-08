import { Readable } from 'node-internal:streams_readable';
import { Writable } from 'node-internal:streams_writable';
import { Buffer } from 'node-internal:internal_buffer';
import { type EventEmitter } from 'node-internal:events';
import {
  normalizePath,
  getValidatedFd,
  isFileHandle,
  type FilePath,
  type Position,
  type WriteSyncOptions,
} from 'node-internal:internal_fs_utils';
import { toPathIfFileURL } from 'node-internal:internal_url';

import * as fs from 'node-internal:internal_fs_callback';

import {
  parseFileMode,
  validateAbortSignal,
  validateBoolean,
  validateFunction,
  validateObject,
  validateString,
  validateUint32,
  validateThisInternalField,
} from 'node-internal:validators';

import type {
  DoubleArgCallback,
  SingleArgCallback,
  ErrorOnlyCallback,
  open as OpenType,
  close as CloseType,
  fsync as FsyncType,
  read as ReadType,
  write as WriteType,
  writev as WritevType,
} from 'node-internal:internal_fs_callback';

import type { FileHandle } from 'node-internal:internal_fs_promises';

import { errorOrDestroy, eos } from 'node-internal:streams_util';

import {
  ERR_INVALID_ARG_VALUE,
  ERR_OUT_OF_RANGE,
  ERR_MISSING_ARGS,
  ERR_METHOD_NOT_IMPLEMENTED,
} from 'node-internal:internal_errors';

import type { ReadAsyncOptions } from 'node:fs';

export interface FsOperations {
  open?: typeof OpenType | undefined;
  close?: typeof CloseType | undefined;
  fsync?: typeof FsyncType | undefined;
  read?: typeof ReadType | undefined;
  write?: typeof WriteType | undefined;
  writev?: typeof WritevType | undefined;
}

export interface RealizedFsOperations {
  open: typeof OpenType;
  close: typeof CloseType;
  fsync: typeof FsyncType;
  read: typeof ReadType;
  write: typeof WriteType;
  writev: typeof WritevType;
}

// Temporary while developing...
/* eslint-disable */

let lazyFs: RealizedFsOperations | undefined;
async function getLazyFs(): Promise<RealizedFsOperations> {
  if (lazyFs == undefined) {
    lazyFs = {
      open: (...args): void => {
        fs.open(...args);
      },
      close: (...args): void => {
        fs.close(...args);
      },
      fsync: (...args): void => {
        fs.fsync(...args);
      },
      read: (...args): void => {
        fs.read(...args);
      },
      write: (...args): void => {
        fs.write(...args);
      },
      writev: (...args): void => {
        fs.writev(...args);
      },
    };
  }
  return lazyFs;
}

const kDefaultFsOperations: RealizedFsOperations = {
  open(
    path: FilePath,
    flags: string | number | SingleArgCallback<number> = 'r',
    mode: string | number | SingleArgCallback<number> = 0o666,
    cb?: SingleArgCallback<number>
  ): void {
    let callback: SingleArgCallback<number>;
    if (typeof flags === 'function') {
      callback = flags;
    } else if (typeof mode === 'function') {
      callback = mode;
    } else if (typeof cb === 'function') {
      callback = cb;
    } else {
      throw new ERR_MISSING_ARGS('fs.open callback');
    }
    validateFunction(callback, 'fs.open callback');

    getLazyFs().then(
      (fs: RealizedFsOperations) => {
        fs.open(path, (err: unknown, fd: number | undefined) => {
          if (err) {
            try {
              callback(err);
            } catch (e: unknown) {
              reportError(e);
            }
            return;
          }
          try {
            callback(null, fd);
          } catch (e: unknown) {
            reportError(e);
          }
        });
      },
      (err: unknown) => {
        try {
          callback(err);
        } catch (e: unknown) {
          reportError(e);
        }
      }
    );
  },
  close(fd: number, cb: ErrorOnlyCallback = () => {}): void {
    getLazyFs().then(
      (fs: RealizedFsOperations) => {
        fs.close(fd, (err: unknown) => {
          if (err) {
            try {
              cb(err);
            } catch (e: unknown) {
              reportError(e);
            }
            return;
          }
          try {
            cb(null);
          } catch (e: unknown) {
            reportError(e);
          }
        });
      },
      (err: unknown) => {
        try {
          cb(err);
        } catch (e: unknown) {
          reportError(e);
        }
      }
    );
  },
  fsync(fd: number, cb: ErrorOnlyCallback = () => {}): void {
    getLazyFs().then(
      (fs: RealizedFsOperations) => {
        fs.fsync(fd, (err: unknown) => {
          if (err) {
            try {
              cb(err);
            } catch (e: unknown) {
              reportError(e);
            }
            return;
          }
          try {
            cb(null);
          } catch (e: unknown) {
            reportError(e);
          }
        });
      },
      (err: unknown) => {
        try {
          cb(err);
        } catch (e: unknown) {
          reportError(e);
        }
      }
    );
  },
  read<T extends NodeJS.ArrayBufferView>(
    fd: number,
    buffer: T | ReadAsyncOptions<T> | DoubleArgCallback<number, T>,
    offset?: ReadAsyncOptions<T> | number | DoubleArgCallback<number, T>,
    length?: null | number | DoubleArgCallback<number, T>,
    position?: Position,
    cb?: DoubleArgCallback<number, T>
  ): void {
    getLazyFs().then(
      (fs: RealizedFsOperations) => {
        fs.read(
          fd,
          buffer,
          offset,
          length,
          position,
          (
            err: unknown,
            bytesRead: number | undefined,
            buffer: T | undefined
          ) => {
            if (err) {
              try {
                cb?.(err);
              } catch (e: unknown) {
                reportError(e);
              }
              return;
            }
            try {
              cb?.(null, bytesRead, buffer);
            } catch (e: unknown) {
              reportError(e);
            }
          }
        );
      },
      (err: unknown) => {
        try {
          cb?.(err);
        } catch (e: unknown) {
          reportError(e);
        }
      }
    );
  },
  write<T extends NodeJS.ArrayBufferView>(
    _fd: number,
    _buffer: T | string,
    _offset?: WriteSyncOptions | Position | DoubleArgCallback<number, T>,
    _length?: number | BufferEncoding | DoubleArgCallback<number, T>,
    _position?: Position | DoubleArgCallback<number, T>,
    _cb?: DoubleArgCallback<number, T>
  ): void {
    throw new ERR_INVALID_ARG_VALUE('fs.write', 'not implemented');
  },
  writev<T extends NodeJS.ArrayBufferView>(
    _fd: number,
    _buffers: T[],
    _position?: Position | SingleArgCallback<number>,
    _cb?: SingleArgCallback<number>
  ): void {
    throw new ERR_INVALID_ARG_VALUE('fs.writev', 'not implemented');
  },
};

export type ReadStreamOptions = {
  encoding?: string | undefined;
  autoClose?: boolean | undefined;
  autoDestroy?: boolean | undefined;
  emitClose?: boolean | undefined;
  start?: number | undefined;
  end?: number | undefined;
  highWaterMark?: number | undefined;
  signal?: AbortSignal | undefined;
  flags?: string | undefined;
  fd?: number | FileHandle | undefined;
  mode?: number | undefined;
  fs?: FsOperations | undefined;
};

const kFs = Symbol('kFs');
const kIsPerformingIO = Symbol('kIsPerformingIO');
const kIoDone = Symbol('kIoDone');
const kHandle = Symbol('kHandle');

// @ts-expect-error TS2323 Cannot redeclare.
export declare class ReadStream extends Readable {
  fd: number | null;
  flags: string;
  path: string;
  mode: number;
  start: number;
  end: number;
  pos: number | undefined;
  bytesRead: number;
  flush: boolean;
  [kFs]: RealizedFsOperations;
  [kIsPerformingIO]: boolean;
  [kHandle]: FileHandle | undefined;
  constructor(path: FilePath, options?: ReadStreamOptions);
  push(chunk: NodeJS.ArrayBufferView | null): boolean;
  close(callback?: ErrorOnlyCallback): void;
}

function construct(this: ReadStream, callback: (err: unknown) => void): void {
  const stream = this;
  if (typeof stream.fd === 'number') {
    callback(null);
    return;
  }

  const ee = stream as unknown as EventEmitter;

  if (typeof (stream as any).open === 'function') {
    // Backwards compat for monkey patching open().
    const orgEmit = ee.emit;
    ee.emit = function (...args): void {
      if (args[0] === 'open') {
        this.emit = orgEmit;
        callback(null);
        Reflect.apply(orgEmit, this, args);
      } else if (args[0] === 'error') {
        this.emit = orgEmit;
        callback(args[1]);
      } else {
        Reflect.apply(orgEmit, this, args);
      }
    };
    (stream as any).open();
    return;
  }
  stream[kFs].open(stream.path, (er: unknown, fd: number | undefined) => {
    if (er) {
      callback(er);
      return;
    }
    if (fd === undefined) {
      callback(new ERR_INVALID_ARG_VALUE('fd', 'undefined'));
      return;
    }
    stream.fd = fd;
    callback(null);
    ee.emit('open', stream.fd);
    ee.emit('ready');
  });
}

function getValidatedFsOptions(fs: FsOperations): RealizedFsOperations {
  validateObject(fs, 'options.fs');

  if (isFileHandle(fs)) {
    const handle = fs as unknown as FileHandle;
    const open = function (..._args: unknown[]): void {
      throw new ERR_METHOD_NOT_IMPLEMENTED('open()');
    };
    const close = function (...args: unknown[]): void {
      const cb = args[args.length - 1] as ErrorOnlyCallback;
      handle.close().then(
        () => cb(null),
        (err: unknown) => cb(err)
      );
    };
    const fsync = function (...args: unknown[]): void {
      const cb = args[args.length - 1] as ErrorOnlyCallback;
      handle.sync().then(
        () => cb(null),
        (err: unknown) => cb(err)
      );
    };
    const read = function (...args: unknown[]): void {
      const cb = args[args.length - 1] as DoubleArgCallback<
        number,
        NodeJS.ArrayBufferView
      >;
      // @ts-expect-error TS2345
      handle.read(...args.slice(0, -1)).then(
        (result: { bytesRead: number; buffer: NodeJS.ArrayBufferView }) => {
          cb(null, result.bytesRead, result.buffer);
        },
        (err: unknown) => cb(err)
      );
    };
    const write = function (...args: unknown[]): void {
      const cb = args[args.length - 1] as DoubleArgCallback<
        number,
        NodeJS.ArrayBufferView
      >;
      // @ts-expect-error TS2556
      handle.write(...args.slice(0, -1)).then(
        (result: { bytesWritten: number; buffer: NodeJS.ArrayBufferView }) => {
          cb(null, result.bytesWritten, result.buffer);
        },
        (err: unknown) => cb(err)
      );
    };
    const writev = function (...args: unknown[]): void {
      const cb = args[args.length - 1] as DoubleArgCallback<
        number,
        NodeJS.ArrayBufferView[]
      >;
      // @ts-expect-error TS2556
      handle.writev(...args.slice(0, -1)).then(
        (result: {
          bytesWritten: number;
          buffers: NodeJS.ArrayBufferView[];
        }) => {
          cb(null, result.bytesWritten, result.buffers);
        },
        (err: unknown) => cb(err)
      );
    };
    return {
      open,
      close,
      fsync,
      read,
      write,
      writev,
    };
  }

  let {
    open = kDefaultFsOperations.open,
    close = kDefaultFsOperations.close,
    fsync = kDefaultFsOperations.fsync,
    read = kDefaultFsOperations.read,
    write = kDefaultFsOperations.write,
    writev = kDefaultFsOperations.writev,
  } = fs as FsOperations;

  validateFunction(open, 'options.fs.open');
  validateFunction(read, 'options.fs.read');
  validateFunction(close, 'options.fs.close');
  validateFunction(fsync, 'options.fs.fsync');
  validateFunction(write, 'options.fs.write');
  validateFunction(writev, 'options.fs.writev');
  return { open, close, fsync, read, write, writev };
}

function readImpl(this: ReadStream, n: number): void {
  n =
    this.pos !== undefined
      ? Math.min(this.end - this.pos + 1, n)
      : Math.min(this.end - this.bytesRead + 1, n);
  if (n <= 0) {
    this.push(null);
    return;
  }

  const buf = Buffer.allocUnsafeSlow(n);
  const ee = this as unknown as EventEmitter;

  this[kIsPerformingIO] = true;
  if (this.fd == null) {
    this.push(null);
    return;
  }
  this[kFs].read(this.fd, buf, 0, n, this.pos, (er, bytesRead, buf) => {
    this[kIsPerformingIO] = false;

    // Tell ._destroy() that it's safe to close the fd now.
    if (this.destroyed) {
      ee.emit(kIoDone, er);
      return;
    }

    if (er) {
      errorOrDestroy(this, er);
      return;
    }

    if (buf == null) {
      errorOrDestroy(this, new ERR_INVALID_ARG_VALUE('buf', 'null'));
      return;
    }

    if (bytesRead != null && bytesRead > 0) {
      if (this.pos !== undefined) {
        this.pos += bytesRead;
      }

      this.bytesRead += bytesRead;

      if (bytesRead !== buf.byteLength) {
        // Slow path. Shrink to fit.
        // Copy instead of slice so that we don't retain
        // large backing buffer for small reads.
        const dst = Buffer.allocUnsafeSlow(bytesRead);
        if (Buffer.isBuffer(buf)) {
          (buf as Buffer).copy(dst, 0, 0, bytesRead);
        } else {
          const buffer = Buffer.from(buf.buffer, buf.byteOffset, bytesRead);
          buffer.copy(dst, 0, 0, bytesRead);
        }
        buf = dst;
      }

      this.push(buf);
    } else {
      this.push(null);
    }
  });
}

function actualCloseImpl(
  stream: ReadStream,
  err: unknown,
  cb: ErrorOnlyCallback
): void {
  if (stream.fd == null) return;
  stream[kFs].close(stream.fd, (er) => {
    cb(er || err);
  });
  stream.fd = null;
}

function closeImpl(
  stream: ReadStream,
  err: unknown,
  cb: ErrorOnlyCallback
): void {
  if (stream.fd == null) {
    cb(err);
  } else if (stream.flush) {
    stream[kFs].fsync(stream.fd, (flushErr) => {
      actualCloseImpl(stream, err || flushErr, cb);
    });
  } else {
    actualCloseImpl(stream, err, cb);
  }
}

function destroyImpl(
  this: ReadStream,
  err: unknown,
  cb: ErrorOnlyCallback
): void {
  // Usually for async IO it is safe to close a file descriptor
  // even when there are pending operations. However, due to platform
  // differences file IO is implemented using synchronous operations
  // running in a thread pool. Therefore, file descriptors are not safe
  // to close while used in a pending read or write operation. Wait for
  // any pending IO (kIsPerformingIO) to complete (kIoDone).
  const ee = this as unknown as EventEmitter;
  if (this[kIsPerformingIO]) {
    ee.once(kIoDone, (er) => {
      closeImpl(this, err || er, cb);
    });
  } else {
    closeImpl(this, err, cb);
  }
}

// @ts-expect-error TS2323 Cannot redeclare.
export function ReadStream(
  this: unknown,
  path: FilePath,
  options: ReadStreamOptions = {}
): ReadStream {
  if (!(this instanceof ReadStream)) {
    return new ReadStream(path, options);
  }
  if (options === null) {
    options = {};
  } else if (typeof options === 'string') {
    options = { encoding: options };
  }

  validateObject(options, 'options');
  const {
    encoding = null,
    autoClose = true,
    emitClose = true,
    start = 0,
    end = Infinity,
    highWaterMark = 64 * 1024,
    signal = null,
    flags = 'r',
    fd = null,
    mode = 0o666,
    fs = kDefaultFsOperations,
  } = options;
  const {
    autoDestroy = options.autoClose === undefined ? true : options.autoClose,
  } = options;

  if (
    encoding !== 'buffer' &&
    encoding !== null &&
    !Buffer.isEncoding(encoding)
  ) {
    throw new ERR_INVALID_ARG_VALUE('options.encoding', encoding);
  }
  validateBoolean(autoClose, 'options.autoClose');
  validateBoolean(emitClose, 'options.emitClose');
  validateUint32(start, 'options.start');
  if (end !== Infinity) {
    validateUint32(end, 'options.end');
    if (start > end) {
      throw new ERR_OUT_OF_RANGE('start', `<= "end" (here: ${end})`, start);
    }
  }
  validateUint32(highWaterMark, 'options.highWaterMark');
  if (signal != null) {
    validateAbortSignal(signal, 'options.signal');
  }

  validateString(flags, 'options.flags');

  // We don't actually use the mode in our implementation. Parsing it is
  // just to ensure that it is validated.
  parseFileMode(mode, 'options.mode', 0o666);

  this[kFs] = getValidatedFsOptions(fs);

  if (fd == null) {
    this.fd = null;
    // Path will be ignored when fd is specified, so it can be falsy
    this.path = toPathIfFileURL(normalizePath(path));
    this.flags = options.flags === undefined ? 'r' : options.flags;
    this.mode = options.mode === undefined ? 0o666 : options.mode;
  } else {
    if (isFileHandle(fd)) {
      if (fs !== kDefaultFsOperations) {
        throw new ERR_METHOD_NOT_IMPLEMENTED('FileHandle with fs');
      }
      this[kHandle] = fd as FileHandle;
      this[kFs] = getValidatedFsOptions(fd as unknown as FsOperations);
      const ee = fd as unknown as EventEmitter;
      ee.on('close', () => this.close());
      this.fd = (fd as FileHandle).fd || null;
    } else {
      this.fd = getValidatedFd(fd as number);
    }
  }

  this.start = start;
  this.end = end;
  this.pos = start;
  this.bytesRead = 0;
  this[kIsPerformingIO] = false;

  Reflect.apply(Readable, this, [
    {
      highWaterMark,
      encoding,
      emitClose,
      autoDestroy,
      signal,
      construct: construct.bind(this),
      read: readImpl.bind(this),
      destroy: destroyImpl.bind(this),
    },
  ]);
  return this;
}
Object.setPrototypeOf(ReadStream.prototype, Readable.prototype);
Object.setPrototypeOf(ReadStream, Readable);

Object.defineProperty(ReadStream.prototype, 'autoClose', {
  get(this: ReadStream): boolean {
    validateThisInternalField(this, kFs, 'ReadStream');
    return this._readableState?.autoDestroy || false;
  },
  set(this: ReadStream, _val: boolean): void {
    validateThisInternalField(this, kFs, 'ReadStream');
    if (this._readableState !== undefined) {
      this._readableState.autoDestroy = _val;
    }
  },
});

ReadStream.prototype.close = function (
  cb: ErrorOnlyCallback = (_err: unknown): void => {}
): void {
  if (typeof cb === 'function') eos(this, cb);
  this.destroy();
};

Object.defineProperty(ReadStream.prototype, 'pending', {
  get(this: ReadStream): boolean {
    return this.fd === null;
  },
  configurable: true,
});

// ======================================================================================

export type WriteStreamOptions = {
  flags?: string;
  encoding?: string;
  fd?: number;
  mode?: number;
  autoClose?: boolean;
};

// @ts-expect-error TS2323 Cannot redeclare.
export declare class WriteStream extends Writable {
  constructor(path: string, options?: WriteStreamOptions);
  close(cb: VoidFunction): void;
  destroySoon(): void;
}

// @ts-expect-error TS2323 Cannot redeclare.
export function WriteStream(
  this: unknown,
  path: string,
  options?: WriteStreamOptions
): WriteStream {
  if (!(this instanceof WriteStream)) {
    return new WriteStream(path, options);
  }

  // TODO(soon): Implement this.

  Reflect.apply(Writable, this, [options]);
  return this;
}
Object.setPrototypeOf(WriteStream.prototype, Writable.prototype);
Object.setPrototypeOf(WriteStream, Writable);

Object.defineProperty(WriteStream.prototype, 'autoClose', {
  get(this: WriteStream): boolean {
    return false;
  },
  set(this: WriteStream, _val: boolean): void {
    // TODO(soon): implement this
  },
});

WriteStream.prototype.close = function (
  this: typeof WriteStream,
  _cb: VoidFunction
): void {
  throw new Error('Not implemented');
};

// There is no shutdown() for files.
WriteStream.prototype.destroySoon = WriteStream.prototype.end;

Object.defineProperty(WriteStream.prototype, 'pending', {
  get(this: WriteStream): boolean {
    // TODO(soon): Implement this
    return false;
  },
  configurable: true,
});

export function createReadStream(
  path: FilePath,
  options: ReadStreamOptions = {}
): ReadStream {
  return new ReadStream(path, options);
}
export function createWriteStream(): void {
  throw new Error('Not implemented');
}
