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

import {
  AbortError,
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_INCOMPATIBLE_OPTION_PAIR,
  ERR_OUT_OF_RANGE,
} from 'node-internal:internal_errors';
import {
  validateAbortSignal,
  validateObject,
  validateBoolean,
  validateInteger,
  validateFunction,
  validateInt32,
  validateUint32,
} from 'node-internal:validators';
import { isDate, isArrayBufferView } from 'node-internal:internal_types';
import {
  F_OK,
  W_OK,
  R_OK,
  X_OK,
  COPYFILE_EXCL,
  COPYFILE_FICLONE,
  O_RDONLY,
  O_APPEND,
  O_CREAT,
  O_RDWR,
  O_EXCL,
  O_SYNC,
  O_TRUNC,
  O_WRONLY,
  S_IFCHR,
  S_IFDIR,
  S_IFREG,
  S_IFLNK,
  S_IFMT,
  S_IFSOCK,
  S_IFIFO,
  S_IFBLK,
  UV_FS_COPYFILE_FICLONE_FORCE,
} from 'node-internal:internal_fs_constants';

import { strictEqual } from 'node-internal:internal_assert';

import { Buffer } from 'node-internal:internal_buffer';
export type FilePath = string | URL | Buffer;

import type { CopyOptions, CopySyncOptions, RmDirOptions } from 'node:fs';

import type { Stat as InternalStat } from 'cloudflare-internal:filesystem';

// A non-public symbol used to ensure that certain constructors cannot
// be called from user-code
export const kBadge = Symbol('kBadge');

export type RawTime = string | number | bigint;
export type SymlinkType = 'dir' | 'file' | 'junction' | null | undefined;

export function normalizePath(path: FilePath, encoding: string = 'utf8'): URL {
  // We treat all of our virtual file system paths as file URLs
  // as a way of normalizing them. Because our file system is
  // fully virtual, we don't need to worry about a number of the
  // issues that real file system paths have and don't need to
  // worry quite as much about strictly checking for null bytes
  // in the path. The URL parsing will take care of those details.
  // We do, however, need to be sensitive to the fact that there
  // are two different URL impls in the runtime that are selected
  // based on compat flags. A worker that is using the legacy URL
  // implementation will end up seeing slightly different behavior
  // here but that's not something we need to worry about for now.
  if (typeof path === 'string') {
    return new URL(path, 'file://');
  } else if (path instanceof URL) {
    return path;
  } else if (Buffer.isBuffer(path)) {
    return new URL(path.toString(encoding), 'file://');
  }
  throw new ERR_INVALID_ARG_TYPE('path', ['string', 'Buffer', 'URL'], path);
}

// The access modes can be any of F_OK, R_OK, W_OK or X_OK. Some might not be
// available on specific systems. They can be used in combination as well
// (F_OK | R_OK | W_OK | X_OK).
export const kMinimumAccessMode = Math.min(F_OK, W_OK, R_OK, X_OK);
export const kMaximumAccessMode = F_OK | W_OK | R_OK | X_OK;

export const kDefaultCopyMode = 0;
// The copy modes can be any of COPYFILE_EXCL, COPYFILE_FICLONE or
// COPYFILE_FICLONE_FORCE. They can be used in combination as well
// (COPYFILE_EXCL | COPYFILE_FICLONE | COPYFILE_FICLONE_FORCE).
export const kMinimumCopyMode = Math.min(
  kDefaultCopyMode,
  COPYFILE_EXCL,
  COPYFILE_FICLONE,
  UV_FS_COPYFILE_FICLONE_FORCE
);
export const kMaximumCopyMode =
  COPYFILE_EXCL | COPYFILE_FICLONE | UV_FS_COPYFILE_FICLONE_FORCE;

// Most platforms don't allow reads or writes >= 2 GiB.
// See https://github.com/libuv/libuv/pull/1501.
export const kIoMaxLength = 2 ** 31 - 1;

// Use 64kb in case the file type is not a regular file and thus do not know the
// actual file size. Increasing the value further results in more frequent over
// allocation for small files and consumes CPU time and memory that should be
// used else wise.
// Use up to 512kb per read otherwise to partition reading big files to prevent
// blocking other threads in case the available threads are all in use.
export const kReadFileUnknownBufferLength = 64 * 1024;
export const kReadFileBufferLength = 512 * 1024;

export const kWriteFileMaxChunkSize = 512 * 1024;

export const kMaxUserId = 2 ** 32 - 1;

export function assertEncoding(encoding: unknown): asserts encoding is string {
  if (encoding && !Buffer.isEncoding(encoding as string)) {
    const reason = 'is invalid encoding';
    throw new ERR_INVALID_ARG_VALUE('encoding', encoding, reason);
  }
}

export function getOptions(
  options: string | Record<string, unknown> | null,
  defaultOptions: Record<string, unknown> = {}
): Record<string, unknown> {
  if (options == null || typeof options === 'function') {
    return defaultOptions;
  }

  if (typeof options === 'string') {
    defaultOptions = { ...defaultOptions };
    defaultOptions.encoding = options;
    options = defaultOptions;
  } else if (typeof options !== 'object') {
    throw new ERR_INVALID_ARG_TYPE('options', ['string', 'Object'], options);
  }

  if (options.encoding !== 'buffer') assertEncoding(options.encoding);

  if (options.signal !== undefined) {
    validateAbortSignal(options.signal, 'options.signal');
  }

  return options;
}

export function copyObject<T>(source: Record<string, T>): Record<string, T> {
  const target: Record<string, T> = {};
  for (const key in source) target[key] = source[key] as T;
  return target;
}

export function getValidMode(
  mode: number | undefined,
  type: 'copyFile' | 'access'
): number {
  let min = kMinimumAccessMode;
  let max = kMaximumAccessMode;
  let def = F_OK;
  if (type === 'copyFile') {
    min = kMinimumCopyMode;
    max = kMaximumCopyMode;
    def = mode || kDefaultCopyMode;
  } else {
    strictEqual(type, 'access');
  }
  if (mode == null) {
    return def;
  }
  validateInteger(mode, 'mode', min, max);
  return mode;
}

const defaultCpOptions: CopyOptions = {
  dereference: false,
  errorOnExist: false,
  force: true,
  preserveTimestamps: false,
  recursive: false,
  verbatimSymlinks: false,
};

export function validateCpOptions(
  _options: unknown
): CopyOptions | CopySyncOptions {
  if (_options === undefined) return { ...defaultCpOptions };
  validateObject(_options, 'options');
  const options: CopyOptions = { ...defaultCpOptions, ..._options };
  validateBoolean(options.dereference, 'options.dereference');
  validateBoolean(options.errorOnExist, 'options.errorOnExist');
  validateBoolean(options.force, 'options.force');
  validateBoolean(options.preserveTimestamps, 'options.preserveTimestamps');
  validateBoolean(options.recursive, 'options.recursive');
  validateBoolean(options.verbatimSymlinks, 'options.verbatimSymlinks');
  options.mode = getValidMode(options.mode, 'copyFile');
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-boolean-literal-compare
  if (options.dereference === true && options.verbatimSymlinks === true) {
    throw new ERR_INCOMPATIBLE_OPTION_PAIR('dereference', 'verbatimSymlinks');
  }
  if (options.filter !== undefined) {
    // eslint-disable-next-line @typescript-eslint/unbound-method
    validateFunction(options.filter, 'options.filter');
  }
  return options;
}

// converts Date or number to a fractional UNIX timestamp
export function toUnixTimestamp(
  time: string | number | Date,
  name: string = 'time'
): string | number | Date {
  // @ts-expect-error TS2367 number to string comparison error.
  if (typeof time === 'string' && +time == time) {
    return +time;
  }
  if (Number.isFinite(time)) {
    // @ts-expect-error TS2365 Number.isFinite does not assert correctly.
    if (time < 0) {
      return Date.now() / 1000;
    }
    return time;
  }
  if (isDate(time)) {
    // Convert to 123.456 UNIX timestamp
    return time.getTime() / 1000;
  }
  throw new ERR_INVALID_ARG_TYPE(name, ['Date', 'Time in seconds'], time);
}

export function stringToFlags(
  flags: number | null | undefined | string,
  name: string = 'flags'
): number {
  if (typeof flags === 'number') {
    validateInt32(flags, name);
    return flags;
  }

  if (flags == null) {
    return O_RDONLY;
  }

  switch (flags) {
    case 'r':
      return O_RDONLY;
    case 'rs': // Fall through.
    case 'sr':
      return O_RDONLY | O_SYNC;
    case 'r+':
      return O_RDWR;
    case 'rs+': // Fall through.
    case 'sr+':
      return O_RDWR | O_SYNC;

    case 'w':
      return O_TRUNC | O_CREAT | O_WRONLY;
    case 'wx': // Fall through.
    case 'xw':
      return O_TRUNC | O_CREAT | O_WRONLY | O_EXCL;

    case 'w+':
      return O_TRUNC | O_CREAT | O_RDWR;
    case 'wx+': // Fall through.
    case 'xw+':
      return O_TRUNC | O_CREAT | O_RDWR | O_EXCL;

    case 'a':
      return O_APPEND | O_CREAT | O_WRONLY;
    case 'ax': // Fall through.
    case 'xa':
      return O_APPEND | O_CREAT | O_WRONLY | O_EXCL;
    case 'as': // Fall through.
    case 'sa':
      return O_APPEND | O_CREAT | O_WRONLY | O_SYNC;

    case 'a+':
      return O_APPEND | O_CREAT | O_RDWR;
    case 'ax+': // Fall through.
    case 'xa+':
      return O_APPEND | O_CREAT | O_RDWR | O_EXCL;
    case 'as+': // Fall through.
    case 'sa+':
      return O_APPEND | O_CREAT | O_RDWR | O_SYNC;
  }

  throw new ERR_INVALID_ARG_VALUE('flags', flags);
}

export function checkAborted(signal?: AbortSignal): void {
  if (signal?.aborted) {
    throw new AbortError(undefined, { cause: signal.reason });
  }
}

const defaultRmdirOptions: RmDirOptions = {
  retryDelay: 100,
  maxRetries: 0,
  recursive: false,
};

export function validateRmdirOptions(
  _options: RmDirOptions | undefined,
  defaults: RmDirOptions = defaultRmdirOptions
): RmDirOptions {
  if (_options === undefined) return defaults;
  validateObject(_options, 'options');

  const options = { ...defaults, ..._options };

  // eslint-disable-next-line @typescript-eslint/no-deprecated
  validateBoolean(options.recursive, 'options.recursive');
  validateInt32(options.retryDelay, 'options.retryDelay', 0);
  validateUint32(options.maxRetries, 'options.maxRetries');

  return options;
}

export type Position = number | null | bigint;

export function validatePosition(
  position: unknown,
  name: string
): asserts position is Position {
  if (typeof position === 'number') {
    validateUint32(position, name);
  } else if (typeof position !== 'bigint' && position !== null) {
    throw new ERR_INVALID_ARG_TYPE(
      name,
      ['integer', 'bigint', 'null'],
      position
    );
  }
}

export function validateOffsetLengthRead(
  offset: number,
  length: number,
  bufferLength: number
): void {
  if (offset < 0) {
    throw new ERR_OUT_OF_RANGE('offset', '>= 0', offset);
  }
  if (length < 0) {
    throw new ERR_OUT_OF_RANGE('length', '>= 0', length);
  }
  if (offset + length > bufferLength) {
    throw new ERR_OUT_OF_RANGE('length', `<= ${bufferLength - offset}`, length);
  }
}

export function getValidatedFd(fd: number, propName: string = 'fd'): number {
  if (Object.is(fd, -0)) {
    return 0;
  }

  validateInt32(fd, propName, 0);

  return fd;
}

export function validateBufferArray(
  buffers: unknown,
  propName: string = 'buffer'
): ArrayBufferView[] {
  if (!Array.isArray(buffers))
    throw new ERR_INVALID_ARG_TYPE(propName, 'ArrayBufferView[]', buffers);

  for (let i = 0; i < buffers.length; i++) {
    if (!isArrayBufferView(buffers[i]))
      throw new ERR_INVALID_ARG_TYPE(propName, 'ArrayBufferView[]', buffers);
  }

  return buffers as ArrayBufferView[];
}

export function validateStringAfterArrayBufferView(
  buffer: unknown,
  name: string
): void {
  if (typeof buffer !== 'string') {
    throw new ERR_INVALID_ARG_TYPE(
      name,
      ['string', 'Buffer', 'TypedArray', 'DataView'],
      buffer
    );
  }
}

export function validateOffsetLengthWrite(
  offset: number,
  length: number,
  byteLength: number
): void {
  if (offset > byteLength) {
    throw new ERR_OUT_OF_RANGE('offset', `<= ${byteLength}`, offset);
  }

  if (length > byteLength - offset) {
    throw new ERR_OUT_OF_RANGE('length', `<= ${byteLength - offset}`, length);
  }

  if (length < 0) {
    throw new ERR_OUT_OF_RANGE('length', '>= 0', length);
  }

  validateInt32(length, 'length', 0);
}

// Our implementation of the Stats class differs a bit from Node.js' in that
// the one in Node.js uses the older function-style class. However, use of
// new fs.Stats(...) has been deprecated in Node.js for quite some time and
// users really aren't supposed to be trying to create their own Stats objects.
// Therefore, we intentionally use a class-style object here and make it an
// error to try to create your own Stats object using the constructor.
export class Stats {
  public dev: number | bigint;
  public ino: number | bigint;
  public mode: number | bigint;
  public nlink: number | bigint;
  public uid: number | bigint;
  public gid: number | bigint;
  public rdev: number | bigint;
  public size: number | bigint;
  public blksize: number | bigint;
  public blocks: number | bigint;
  public atimeMs: number | bigint;
  public mtimeMs: number | bigint;
  public ctimeMs: number | bigint;
  public birthtimeMs: number | bigint;
  public atimeNs?: bigint;
  public mtimeNs?: bigint;
  public ctimeNs?: bigint;
  public birthtimeNs?: bigint;
  public atime: Date;
  public mtime: Date;
  public ctime: Date;
  public birthtime: Date;

  public constructor(
    badge: symbol,
    stat: InternalStat,
    options: { bigint: boolean }
  ) {
    // The kBadge symbol is never exported for users. We use it as an internal
    // marker to ensure that only internal code can create a Stats object using
    // the constructor.
    if (badge !== kBadge) {
      throw new TypeError('Illegal constructor');
    }

    // All nodes are always readable
    this.mode = 0o444;
    if (stat.writable) {
      this.mode |= 0o222; // writable
    }

    if (stat.device) {
      this.mode |= S_IFCHR;
    } else {
      switch (stat.type) {
        case 'file':
          this.mode |= S_IFREG;
          break;
        case 'directory':
          this.mode |= S_IFDIR;
          break;
        case 'symlink':
          this.mode |= S_IFLNK;
          break;
      }
    }

    if (options.bigint) {
      this.dev = BigInt(stat.device);
      this.size = BigInt(stat.size);

      this.atimeNs = 0n;
      this.mtimeNs = stat.lastModified;
      this.ctimeNs = stat.lastModified;
      this.birthtimeNs = stat.created;
      this.atimeMs = this.atimeNs / 1_000_000n;
      this.mtimeMs = this.mtimeNs / 1_000_000n;
      this.ctimeMs = this.ctimeNs / 1_000_000n;
      this.birthtimeMs = this.birthtimeNs / 1_000_000n;
      this.atime = new Date(Number(this.atimeMs));
      this.mtime = new Date(Number(this.mtimeMs));
      this.ctime = new Date(Number(this.ctimeMs));
      this.birthtime = new Date(Number(this.birthtimeMs));

      // We have no meaningful definition of these values.
      this.ino = 0n;
      this.nlink = 1n;
      this.uid = 0n;
      this.gid = 0n;
      this.rdev = 0n;
      this.blksize = 0n;
      this.blocks = 0n;
    } else {
      this.dev = Number(stat.device);
      this.size = stat.size;

      this.atimeMs = 0;
      this.mtimeMs = Number(stat.lastModified) / 1_000_000;
      this.ctimeMs = Number(stat.lastModified) / 1_000_000;
      this.birthtimeMs = Number(stat.created) / 1_000_000;
      this.atime = new Date(this.atimeMs);
      this.mtime = new Date(this.mtimeMs);
      this.ctime = new Date(this.ctimeMs);
      this.birthtime = new Date(this.birthtimeMs);

      // We have no meaningful definition of these values.
      this.ino = 0;
      this.nlink = 1;
      this.uid = 0;
      this.gid = 0;
      this.rdev = 0;
      this.blksize = 0;
      this.blocks = 0;
    }
  }

  public isBlockDevice(): boolean {
    return (Number(this.mode) & S_IFMT) === S_IFBLK;
  }

  public isCharacterDevice(): boolean {
    return (Number(this.mode) & S_IFMT) === S_IFCHR;
  }

  public isDirectory(): boolean {
    return (Number(this.mode) & S_IFMT) === S_IFDIR;
  }

  public isFIFO(): boolean {
    return (Number(this.mode) & S_IFMT) === S_IFIFO;
  }

  public isFile(): boolean {
    return (Number(this.mode) & S_IFMT) === S_IFREG;
  }

  public isSocket(): boolean {
    return (Number(this.mode) & S_IFMT) === S_IFSOCK;
  }

  public isSymbolicLink(): boolean {
    return (Number(this.mode) & S_IFMT) === S_IFLNK;
  }
}
