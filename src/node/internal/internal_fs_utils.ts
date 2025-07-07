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
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_INCOMPATIBLE_OPTION_PAIR,
} from 'node-internal:internal_errors';
import {
  validateAbortSignal,
  validateObject,
  validateBoolean,
  validateInteger,
  validateFunction,
  validateInt32,
  validateUint32,
  validateEncoding,
  parseFileMode,
} from 'node-internal:validators';
import { isArrayBufferView } from 'node-internal:internal_types';
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

import type {
  CopyOptions,
  CopySyncOptions,
  MakeDirectoryOptions,
  OpenDirOptions,
  ReadSyncOptions,
  RmDirOptions,
  RmOptions,
  WriteFileOptions,
} from 'node:fs';

import type { Stat as InternalStat } from 'cloudflare-internal:filesystem';

// A non-public symbol used to ensure that certain constructors cannot
// be called from user-code
export const kBadge = Symbol('kBadge');
export const kFileHandle = Symbol('kFileHandle');

export function isFileHandle(object: unknown): boolean {
  if (typeof object !== 'object' || object === null) return false;
  return Reflect.has(object, kFileHandle);
}

export type RawTime = string | number | bigint;
export type SymlinkType = 'dir' | 'file' | 'junction' | null | undefined;

// Normalizes the input time to a Date object.
export function getDate(time: RawTime | Date): Date {
  if (typeof time === 'number') {
    return new Date(time);
  } else if (typeof time === 'bigint') {
    return new Date(Number(time));
  } else if (typeof time === 'string') {
    return new Date(time);
  } else if (time instanceof Date) {
    return time;
  }
  throw new ERR_INVALID_ARG_TYPE(
    'time',
    ['string', 'number', 'bigint', 'Date'],
    time
  );
}

// Normalizes the input file path to a URL object.
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

export const kMaxUserId = 2 ** 32 - 1;

// In Node.js async callback APIs, input arguments are always validated
// with input validation errors thrown synchronously. Only errors that
// occur during the actual operation (e.g. file not found) are reported
// via the callback. The validateAccessArgs function is used by both the
// accessSync and access-with-callback APIs to validate the input args.
export function validateAccessArgs(
  rawPath: FilePath,
  mode: number
): { path: URL; mode: number } {
  return {
    path: normalizePath(rawPath),
    mode: validateMode(mode),
  };
}

export function validateChownArgs(
  pathOrFd: FilePath | number,
  uid: number,
  gid: number
): { pathOrFd: URL | number; uid: number; gid: number } {
  validateInteger(uid, 'uid', -1, kMaxUserId);
  validateInteger(gid, 'gid', -1, kMaxUserId);
  if (typeof pathOrFd === 'number') {
    return {
      pathOrFd: getValidatedFd(pathOrFd, 'fd'),
      uid,
      gid,
    };
  }
  return {
    pathOrFd: normalizePath(pathOrFd),
    uid,
    gid,
  };
}

export function validateStatArgs(
  path: number | FilePath,
  options: {
    bigint?: boolean | undefined;
    throwIfNoEntry?: boolean | undefined;
  } = {},
  isfstat = false
): { pathOrFd: number | URL; bigint: boolean; throwIfNoEntry: boolean } {
  validateObject(options, 'options');
  const { bigint = false, throwIfNoEntry = true } = options;
  validateBoolean(bigint, 'options.bigint');
  validateBoolean(throwIfNoEntry, 'options.throwIfNoEntry');
  if (typeof path === 'number') {
    return {
      pathOrFd: getValidatedFd(path, 'fd'),
      bigint,
      throwIfNoEntry,
    };
  }
  if (isfstat) {
    throw new ERR_INVALID_ARG_TYPE('fd', 'number', path);
  }
  return {
    pathOrFd: normalizePath(path),
    bigint,
    throwIfNoEntry,
  };
}
export function validateChmodArgs(
  pathOrFd: FilePath | number,
  mode: number | string
): { pathOrFd: URL | number; mode: number } {
  const actualMode = parseFileMode(mode, 'mode');
  if (typeof pathOrFd === 'number') {
    return {
      pathOrFd: getValidatedFd(pathOrFd, 'fd'),
      mode: actualMode,
    };
  }
  return {
    pathOrFd: normalizePath(pathOrFd),
    mode: actualMode,
  };
}

export function validateMkDirArgs(
  path: FilePath,
  options: number | MakeDirectoryOptions
): { path: URL; recursive: boolean } {
  const { recursive = false, mode = 0o777 } = ((): MakeDirectoryOptions => {
    if (typeof options === 'number') {
      return { mode: options };
    } else {
      validateObject(options, 'options');
      return options;
    }
  })();

  validateBoolean(recursive, 'options.recursive');

  // We don't implement the mode option in any meaningful way. We just validate it.
  parseFileMode(mode, 'mode');

  return {
    path: normalizePath(path),
    recursive,
  };
}

export function validateRmArgs(
  path: FilePath,
  options: RmOptions
): { path: URL; recursive: boolean; force: boolean } {
  validateObject(options, 'options');
  const {
    force = false,
    maxRetries = 0,
    recursive = false,
    retryDelay = 0,
  } = options;
  // We do not implement the maxRetries or retryDelay options in any meaningful
  // way. We just validate them.
  validateBoolean(force, 'options.force');
  validateUint32(maxRetries, 'options.maxRetries');
  validateBoolean(recursive, 'options.recursive');
  validateUint32(retryDelay, 'options.retryDelay');
  return {
    path: normalizePath(path),
    recursive,
    force,
  };
}

export function validateRmDirArgs(
  path: FilePath,
  options: RmDirOptions
): { path: URL; recursive: boolean } {
  validateObject(options, 'options');
  const { maxRetries = 0, recursive = false, retryDelay = 0 } = options;
  // We do not implement the maxRetries or retryDelay options in any meaningful
  // way. We just validate them.
  validateUint32(maxRetries, 'options.maxRetries');
  validateBoolean(recursive, 'options.recursive');
  validateUint32(retryDelay, 'options.retryDelay');
  return {
    path: normalizePath(path),
    recursive,
  };
}

// We could use the @types/node definition here but it's a bit overly
// complex for our needs here.
export type ReadDirOptions = {
  encoding?: BufferEncoding | 'buffer' | null | undefined;
  withFileTypes?: boolean | undefined;
  recursive?: boolean | undefined;
};

export function validateReaddirArgs(
  path: FilePath,
  options: ReadDirOptions
): {
  path: URL;
  encoding: BufferEncoding | 'buffer';
  withFileTypes: boolean;
  recursive: boolean;
} {
  validateObject(options, 'options');
  const {
    encoding = 'utf8',
    withFileTypes = false,
    recursive = false,
  } = options;
  if (encoding !== 'buffer' && !Buffer.isEncoding(encoding)) {
    throw new ERR_INVALID_ARG_VALUE('options.encoding', encoding);
  }
  validateBoolean(withFileTypes, 'options.withFileTypes');
  validateBoolean(recursive, 'options.recursive');
  return {
    path: normalizePath(path),
    encoding,
    withFileTypes,
    recursive,
  };
}

export function validateOpendirArgs(
  path: FilePath,
  options: OpenDirOptions
): {
  path: URL;
  encoding: BufferEncoding | null | 'buffer';
  recursive: boolean;
} {
  validateObject(options, 'options');
  const { encoding = 'utf8', bufferSize = 32, recursive = false } = options;
  if (!Buffer.isEncoding(encoding) && encoding !== 'buffer') {
    throw new ERR_INVALID_ARG_VALUE('options.encoding', encoding);
  }

  // We don't implement the bufferSize option in any meaningful way but we
  // do at least validate it.
  validateUint32(bufferSize, 'options.bufferSize');
  validateBoolean(recursive, 'options.recursive');
  return {
    path: normalizePath(path),
    encoding: encoding as BufferEncoding,
    recursive,
  };
}

export type WriteSyncOptions = {
  offset?: number | undefined;
  length?: number | undefined;
  position?: Position | undefined;
};

export function validateWriteArgs(
  fd: number,
  buffer: NodeJS.ArrayBufferView | string,
  offsetOrOptions: WriteSyncOptions | Position | undefined,
  length: number | BufferEncoding | null | undefined,
  position: Position | undefined
): { fd: number; buffer: Buffer[]; position: Position } {
  fd = getValidatedFd(fd);

  let offset: number | undefined | null = offsetOrOptions as number;
  if (isArrayBufferView(buffer)) {
    if (typeof offsetOrOptions === 'object' && offsetOrOptions != null) {
      ({
        offset = 0,
        length = buffer.byteLength - offset,
        position = null,
      } = (offsetOrOptions as WriteSyncOptions | null) || {});
    }
    position ??= null;
    offset ??= buffer.byteOffset;

    validateInteger(offset, 'offset', 0);
    validatePosition(position, 'position');

    length ??= buffer.byteLength - offset;

    validateInteger(length, 'length', 0);

    // Validate that the offset + length do not exceed the buffer's byte length.
    if (offset + length > buffer.byteLength) {
      throw new ERR_INVALID_ARG_VALUE('offset', offset, 'out of bounds');
    }

    return {
      fd,
      buffer: [Buffer.from(buffer.buffer, offset, length)],
      position,
    };
  }

  validateStringAfterArrayBufferView(buffer, 'buffer');

  // In this case, offsetOrOptions must either be a number, bigint, or null.
  validatePosition(offsetOrOptions, 'position');
  position = offsetOrOptions;

  // In this instance, buffer is a string and the length arg specifies
  // the encoding to use.
  validateEncoding(buffer, length as string);
  return {
    fd,
    buffer: [Buffer.from(buffer, length as string /* encoding */)],
    position,
  };
}

export function validateWriteFileArgs(
  path: number | FilePath,
  data: string | ArrayBufferView,
  options: BufferEncoding | null | WriteFileOptions
): {
  path: number | URL;
  data: NodeJS.ArrayBufferView;
  append: boolean;
  exclusive: boolean;
} {
  if (typeof path === 'number') {
    path = getValidatedFd(path);
  } else {
    path = normalizePath(path);
  }

  if (typeof options === 'string' || options == null) {
    options = { encoding: options as BufferEncoding | null };
  }

  validateObject(options, 'options');
  const {
    encoding = 'utf8',
    mode = 0o666,
    flag = 'w',
    flush = false,
  } = options;
  if (encoding !== 'buffer' && !Buffer.isEncoding(encoding)) {
    throw new ERR_INVALID_ARG_VALUE('options.encoding', encoding);
  }
  validateBoolean(flush, 'options.flush');
  parseFileMode(mode, 'options.mode', 0o666);
  const newFlag = stringToFlags(flag as string);

  const append = Boolean(newFlag & O_APPEND);
  const write = Boolean(newFlag & O_WRONLY || newFlag & O_RDWR) || append;
  const exclusive = Boolean(newFlag & O_EXCL);

  if (!write) {
    throw new ERR_INVALID_ARG_VALUE(
      'flag',
      flag,
      'must be indicate write or append'
    );
  }

  // We're not currently implementing the exclusive flag. We're validating
  // it here just to use it so the compiler doesn't complain.
  validateBoolean(exclusive, 'options.exclusive');

  if (typeof data === 'string') {
    data = Buffer.from(data, encoding);
  }

  if (!isArrayBufferView(data)) {
    throw new ERR_INVALID_ARG_TYPE(
      'data',
      ['string', 'Buffer', 'TypedArray', 'DataView'],
      data
    );
  }

  return {
    path,
    data: data as NodeJS.ArrayBufferView,
    append,
    exclusive,
  };
}

export function validateReadArgs(
  fd: number,
  buffer: NodeJS.ArrayBufferView,
  offsetOrOptions: ReadSyncOptions | number | null,
  length: number | undefined,
  position: Position | undefined
): { fd: number; buffer: Buffer[]; length: number; position: Position } {
  fd = getValidatedFd(fd);

  // Great fun with polymorphism here. We're going to normalize the arguments
  // to match the first signature (fd, buffer, offset, length, position).
  //
  // If the third argument is an object, then we will pull the offset, length,
  // and position from it, ignoring the remaining arguments. If the third
  // argument is a number, then we will use it as the offset and pull the
  // length and position from the fourth and fifth arguments. If the third
  // position is any other type, then we will throw an error.

  if (!isArrayBufferView(buffer)) {
    throw new ERR_INVALID_ARG_TYPE(
      'buffer',
      ['Buffer', 'TypedArray', 'DataView'],
      buffer
    );
  }

  let actualOffset = buffer.byteOffset;
  let actualLength = buffer.byteLength;
  let actualPosition = position;

  // Handle the case where the third argument is an options object
  if (offsetOrOptions != null && typeof offsetOrOptions === 'object') {
    const {
      offset = 0,
      length = buffer.byteLength - offset,
      position = null,
    } = offsetOrOptions;
    actualOffset = offset;
    actualLength = length;
    actualPosition = position;
  }
  // Handle the case where the third argument is a number (offset)
  else if (typeof offsetOrOptions === 'number') {
    actualOffset = offsetOrOptions;
    actualLength = length ?? buffer.byteLength - actualOffset;
    actualPosition = position;
  } else {
    throw new ERR_INVALID_ARG_TYPE(
      'offset',
      ['number', 'object'],
      offsetOrOptions
    );
  }

  validateUint32(actualOffset, 'offset');
  validateUint32(actualLength, 'length');
  validatePosition(actualPosition, 'position');

  // The actualOffset plus actualLength must not exceed the buffer's byte length.
  if (actualOffset + actualLength > buffer.byteLength) {
    throw new ERR_INVALID_ARG_VALUE('offset', actualOffset, 'out of bounds');
  }

  return {
    fd,
    buffer: [Buffer.from(buffer.buffer, actualOffset, actualLength)],
    length: actualLength,
    position: actualPosition,
  };
}

// Validate the mode argument for either copyFile or access operations.
// The mode argument is a bitmask that can be used to specify the access
// permissions for a file. The valid modes depend on the operation type:
// - For copyFile, the valid modes are COPYFILE_EXCL, COPYFILE_FICLONE, and
//   COPYFILE_FICLONE_FORCE, with a default of 0.
// - For access, the valid modes are F_OK, R_OK, W_OK, and X_OK, with a
//   default of F_OK.
// In either case, the mode must be a valid bitmask integer within the
// a given range. If the mode is not provided, the default value is used.
// Throws ERR_INVALID_ARG_TYPE if the mode is not a valid integer, or
// ERR_OUT_OF_RANGE if the mode is outside the valid range.
function validateMode(
  mode: number | undefined,
  type: 'copyFile' | 'access' = 'access'
): number {
  // The access modes can be any of F_OK, R_OK, W_OK or X_OK. Some might not be
  // available on specific systems. They can be used in combination as well
  // (F_OK | R_OK | W_OK | X_OK).
  let min = Math.min(F_OK, W_OK, R_OK, X_OK);
  let max = F_OK | W_OK | R_OK | X_OK;
  let def = F_OK;
  if (type === 'copyFile') {
    // The copy modes can be any of COPYFILE_EXCL, COPYFILE_FICLONE or
    // COPYFILE_FICLONE_FORCE. They can be used in combination as well
    // (COPYFILE_EXCL | COPYFILE_FICLONE | COPYFILE_FICLONE_FORCE).
    min = Math.min(
      0,
      COPYFILE_EXCL,
      COPYFILE_FICLONE,
      UV_FS_COPYFILE_FICLONE_FORCE
    );
    max = COPYFILE_EXCL | COPYFILE_FICLONE | UV_FS_COPYFILE_FICLONE_FORCE;
    def = mode || 0;
  } else {
    strictEqual(type, 'access');
  }
  mode ??= def;
  validateInteger(mode, 'mode', min, max);
  return mode;
}

function assertEncoding(encoding: unknown): asserts encoding is string {
  if (
    encoding &&
    encoding !== 'buffer' &&
    !Buffer.isEncoding(encoding as string)
  ) {
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
  options.mode = validateMode(options.mode, 'copyFile');
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

function validateStringAfterArrayBufferView(
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

// Our implementation of the Stats class differs a bit from Node.js' in that
// the one in Node.js uses the older function-style class. However, use of
// new fs.Stats(...) has been deprecated in Node.js for quite some time and
// users really aren't supposed to be trying to create their own Stats objects.
// Therefore, we intentionally use a class-style object here and make it an
// error to try to create your own Stats object using the constructor.
export class Stats {
  dev: number | bigint;
  ino: number | bigint;
  mode: number | bigint;
  nlink: number | bigint;
  uid: number | bigint;
  gid: number | bigint;
  rdev: number | bigint;
  size: number | bigint;
  blksize: number | bigint;
  blocks: number | bigint;
  atimeMs: number | bigint;
  mtimeMs: number | bigint;
  ctimeMs: number | bigint;
  birthtimeMs: number | bigint;
  atimeNs?: bigint;
  mtimeNs?: bigint;
  ctimeNs?: bigint;
  birthtimeNs?: bigint;
  atime: Date;
  mtime: Date;
  ctime: Date;
  birthtime: Date;

  constructor(badge: symbol, stat: InternalStat, options: { bigint: boolean }) {
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

      this.mode = BigInt(this.mode);
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

  isBlockDevice(): boolean {
    return (Number(this.mode) & S_IFMT) === S_IFBLK;
  }

  isCharacterDevice(): boolean {
    return (Number(this.mode) & S_IFMT) === S_IFCHR;
  }

  isDirectory(): boolean {
    return (Number(this.mode) & S_IFMT) === S_IFDIR;
  }

  isFIFO(): boolean {
    return (Number(this.mode) & S_IFMT) === S_IFIFO;
  }

  isFile(): boolean {
    return (Number(this.mode) & S_IFMT) === S_IFREG;
  }

  isSocket(): boolean {
    return (Number(this.mode) & S_IFMT) === S_IFSOCK;
  }

  isSymbolicLink(): boolean {
    return (Number(this.mode) & S_IFMT) === S_IFLNK;
  }
}
