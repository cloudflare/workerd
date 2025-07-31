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
import * as fssync from 'node-internal:internal_fs_sync';
import { default as cffs } from 'cloudflare-internal:filesystem';
import type {
  FStatOptions,
  MkdirTempSyncOptions,
  ReadDirResult,
  ReadFileSyncOptions,
  ReadLinkSyncOptions,
  StatOptions,
} from 'node-internal:internal_fs_sync';
import {
  validatePosition,
  getDate,
  validateAccessArgs,
  validateChownArgs,
  validateChmodArgs,
  validateStatArgs,
  validateMkDirArgs,
  validateOpendirArgs,
  validateRmArgs,
  validateRmDirArgs,
  validateReaddirArgs,
  validateWriteArgs,
  validateWriteFileArgs,
  normalizePath,
  Stats,
  type FilePath,
  type Position,
  type RawTime,
  type SymlinkType,
  type ReadDirOptions,
  type WriteSyncOptions,
  getValidatedFd,
  validateBufferArray,
  stringToFlags,
} from 'node-internal:internal_fs_utils';
import {
  F_OK,
  COPYFILE_EXCL,
  COPYFILE_FICLONE,
  COPYFILE_FICLONE_FORCE,
} from 'node-internal:internal_fs_constants';
import {
  ERR_EBADF,
  ERR_ENOENT,
  ERR_EEXIST,
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_UNSUPPORTED_OPERATION,
} from 'node-internal:internal_errors';
import { type Dir } from 'node-internal:internal_fs';
import { Buffer } from 'node-internal:internal_buffer';
import { isArrayBufferView } from 'node-internal:internal_types';
import {
  parseFileMode,
  validateBoolean,
  validateObject,
  validateOneOf,
  validateUint32,
} from 'node-internal:validators';
import type {
  BigIntStatsFs,
  CopySyncOptions,
  GlobOptions,
  GlobOptionsWithFileTypes,
  GlobOptionsWithoutFileTypes,
  MakeDirectoryOptions,
  OpenDirOptions,
  ReadAsyncOptions,
  RmOptions,
  RmDirOptions,
  StatsFs,
  WriteFileOptions,
} from 'node:fs';

export type ErrorOnlyCallback = (err: unknown) => void;
export type SingleArgCallback<T> = (err: unknown, result?: T) => void;
export type DoubleArgCallback<T, U> = (
  err: unknown,
  result1?: T,
  result2?: U
) => void;

function callWithErrorOnlyCallback(
  fn: () => void,
  callback: undefined | ErrorOnlyCallback
): void {
  if (typeof callback !== 'function') {
    throw new ERR_INVALID_ARG_TYPE('callback', ['function'], callback);
  }
  try {
    fn();
    // Note that any errors thrown by the callback will be "handled" by passing
    // them along to the reportError function, which logs them and triggers the
    // global "error" event.
    queueMicrotask(() => {
      callback(null);
    });
  } catch (err) {
    queueMicrotask(() => {
      callback(err);
    });
  }
}

function callWithSingleArgCallback<T>(
  fn: () => T,
  callback: undefined | SingleArgCallback<T>
): void {
  if (typeof callback !== 'function') {
    throw new ERR_INVALID_ARG_TYPE('callback', ['function'], callback);
  }
  try {
    const result = fn();
    queueMicrotask(() => {
      callback(null, result);
    });
  } catch (err) {
    queueMicrotask(() => {
      callback(err);
    });
  }
}

export function access(
  path: FilePath,
  modeOrCallback: number | ErrorOnlyCallback = F_OK,
  callback?: ErrorOnlyCallback
): void {
  let mode: number;
  if (typeof modeOrCallback === 'function') {
    callback = modeOrCallback;
    mode = F_OK;
  } else {
    mode = modeOrCallback;
  }

  const { path: actualPath, mode: actualMode } = validateAccessArgs(path, mode);

  callWithErrorOnlyCallback(() => {
    fssync.accessSyncImpl(actualPath, actualMode, true);
  }, callback);
}

export type ExistsCallback = (result: boolean) => void;

export function exists(path: FilePath, callback: ExistsCallback): void {
  // With the other methods we perform the method and *then* pass the results
  // back to the callback using queueMicrotask. Here, however, we wait to
  // perform the method until we are in the mcirotask. This is so we can
  // best avoid the race condition that has long existed with the exists
  // method in Node.js where the file may be deleted between the time we
  // check for its existence and the time we call the callback.
  queueMicrotask(() => {
    callback(fssync.existsSync(path));
  });
}

export function appendFile(
  path: number | FilePath,
  data: string | ArrayBufferView,
  optionsOrCallback: WriteFileOptions | ErrorOnlyCallback,
  callback?: ErrorOnlyCallback
): void {
  let options: WriteFileOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {
      encoding: 'utf8',
      mode: 0o666,
      flag: 'a',
      flush: false,
    };
  } else {
    options = optionsOrCallback;
  }
  writeFile(path, data, options, callback);
}

export function chmod(
  path: FilePath,
  mode: number | string,
  callback: ErrorOnlyCallback
): void {
  const { pathOrFd } = validateChmodArgs(path, mode);
  callWithErrorOnlyCallback(() => {
    if (cffs.stat(pathOrFd as URL, { followSymlinks: true }) == null) {
      throw new ERR_ENOENT((pathOrFd as URL).pathname, { syscall: 'chmod' });
    }
  }, callback);
}

export function chown(
  path: FilePath,
  uid: number,
  gid: number,
  callback: ErrorOnlyCallback
): void {
  const { pathOrFd } = validateChownArgs(path, uid, gid);
  callWithErrorOnlyCallback(() => {
    if (cffs.stat(pathOrFd as URL, { followSymlinks: true }) == null) {
      throw new ERR_ENOENT((path as URL).pathname, { syscall: 'chown' });
    }
  }, callback);
}

export function close(
  fd: number,
  callback: ErrorOnlyCallback = () => {}
): void {
  fd = getValidatedFd(fd);
  callWithErrorOnlyCallback(() => {
    fssync.closeSync(fd);
  }, callback);
}

export function copyFile(
  src: FilePath,
  dest: FilePath,
  modeOrCallback: number | ErrorOnlyCallback = 0,
  callback?: ErrorOnlyCallback
): void {
  let mode: number;
  if (typeof modeOrCallback === 'function') {
    callback = modeOrCallback;
    mode = 0;
  } else {
    mode = modeOrCallback;
  }
  const normalizedSrc = normalizePath(src);
  const normalizedDest = normalizePath(dest);

  validateOneOf(mode, 'mode', [
    0,
    COPYFILE_EXCL,
    COPYFILE_FICLONE_FORCE,
    COPYFILE_FICLONE,
  ]);
  if (mode & COPYFILE_FICLONE_FORCE) {
    throw new ERR_UNSUPPORTED_OPERATION();
  }
  if (mode & COPYFILE_EXCL && fssync.existsSync(dest)) {
    throw new ERR_EEXIST({ syscall: 'copyFile' });
  }

  callWithErrorOnlyCallback(() => {
    fssync.copyFileSync(normalizedSrc, normalizedDest, mode);
  }, callback);
}

export function cp(
  src: FilePath,
  dest: FilePath,
  optionsOrCallback: CopySyncOptions | ErrorOnlyCallback,
  callback?: ErrorOnlyCallback
): void {
  let options: CopySyncOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }

  validateObject(options, 'options');
  const {
    dereference = false,
    errorOnExist = false,
    force = true,
    filter,
    mode = 0,
    preserveTimestamps = false,
    recursive = false,
    verbatimSymlinks = false,
  } = options;

  validateBoolean(dereference, 'options.dereference');
  validateBoolean(errorOnExist, 'options.errorOnExist');
  validateBoolean(force, 'options.force');
  validateBoolean(preserveTimestamps, 'options.preserveTimestamps');
  validateBoolean(recursive, 'options.recursive');
  validateBoolean(verbatimSymlinks, 'options.verbatimSymlinks');
  validateUint32(mode, 'options.mode');

  if (mode & COPYFILE_FICLONE_FORCE) {
    throw new ERR_INVALID_ARG_VALUE(
      'options.mode',
      'COPYFILE_FICLONE_FORCE is not supported'
    );
  }

  if (filter !== undefined) {
    if (typeof filter !== 'function') {
      throw new ERR_INVALID_ARG_TYPE('options.filter', 'function', filter);
    }
    // We do not implement the filter option currently. There's a bug in the Node.js
    // implementation of fs.cp and the option.filter in which non-UTF-8 encoded file
    // names are not handled correctly and the option.filter fails when the src or
    // dest is passed in as a Buffer. Fixing this bug in Node.js will require a breaking
    // change to the API or a new API that appropriately handles Buffer inputs and non
    // UTF-8 encoded names. We want to avoid implementing the filter option for now
    // until Node.js settles on a better implementation and API.
    throw new ERR_UNSUPPORTED_OPERATION();
  }

  const exclusive = Boolean(mode & COPYFILE_EXCL);
  // We're not current implementing the exclusive flag. We're validating
  // it here just to use it so the compiler doesn't complain.
  validateBoolean(exclusive, '');

  // We're not current implementing verbatimSymlinks in any meaningful way.
  // Our symlinks are always fully qualfied. That is, they always point to
  // an absolute path and never to a relative path, so there is no distinction
  // between verbatimSymlinks and non-verbatimSymlinks. We validate the option
  // value above but otherwise we ignore it.

  src = normalizePath(src);
  dest = normalizePath(dest);

  callWithErrorOnlyCallback(() => {
    cffs.cp(src, dest, {
      deferenceSymlinks: dereference,
      recursive,
      force,
      errorOnExist,
    });
  }, callback);
}

export function fchmod(
  fd: number,
  mode: string | number,
  callback: ErrorOnlyCallback
): void {
  fd = getValidatedFd(fd);
  parseFileMode(mode, 'mode');
  callWithErrorOnlyCallback(() => {
    fssync.fchmodSync(fd, mode);
  }, callback);
}

export function fchown(
  fd: number,
  uid: number,
  gid: number,
  callback: ErrorOnlyCallback
): void {
  const { pathOrFd } = validateChownArgs(fd, uid, gid);
  callWithErrorOnlyCallback(() => {
    if (cffs.stat(pathOrFd as URL, { followSymlinks: false }) == null) {
      throw new ERR_EBADF({ syscall: 'fchown' });
    }
  }, callback);
}

export function fdatasync(fd: number, callback: ErrorOnlyCallback): void {
  getValidatedFd(fd);
  callWithErrorOnlyCallback(() => {
    fssync.fdatasyncSync(fd);
  }, callback);
}

export function fstat(
  fd: number,
  optionsOrCallback: SingleArgCallback<Stats> | FStatOptions,
  callback?: SingleArgCallback<Stats>
): void {
  let options: FStatOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = { bigint: false };
  } else {
    options = optionsOrCallback;
  }
  validateStatArgs(fd, options, true /* is fstat */);
  callWithSingleArgCallback(() => fssync.fstatSync(fd, options), callback);
}

export function fsync(
  fd: number,
  callback: ErrorOnlyCallback = () => {}
): void {
  getValidatedFd(fd);
  callWithErrorOnlyCallback(() => {
    fssync.fsyncSync(fd);
  }, callback);
}

export function ftruncate(
  fd: number,
  lenOrCallback: number | ErrorOnlyCallback,
  callback?: ErrorOnlyCallback
): void {
  let len: number;
  if (typeof lenOrCallback === 'function') {
    callback = lenOrCallback;
    len = 0;
  } else {
    len = lenOrCallback;
  }
  callWithErrorOnlyCallback(() => {
    fssync.ftruncateSync(fd, len);
  }, callback);
}

export function futimes(
  fd: number,
  atime: RawTime | Date,
  mtime: RawTime | Date,
  callback: ErrorOnlyCallback
): void {
  fd = getValidatedFd(fd);
  atime = getDate(atime);
  mtime = getDate(mtime);
  callWithErrorOnlyCallback(() => {
    fssync.futimesSync(fd, atime, mtime);
  }, callback);
}

export function lchmod(
  path: FilePath,
  mode: string | number,
  callback: ErrorOnlyCallback
): void {
  const { pathOrFd } = validateChmodArgs(path, mode);
  callWithErrorOnlyCallback(() => {
    if (cffs.stat(pathOrFd as URL, { followSymlinks: false }) == null) {
      throw new ERR_ENOENT((pathOrFd as URL).pathname, { syscall: 'lchmod' });
    }
  }, callback);
}

export function lchown(
  path: FilePath,
  uid: number,
  gid: number,
  callback: ErrorOnlyCallback
): void {
  const { pathOrFd } = validateChownArgs(path, uid, gid);
  callWithErrorOnlyCallback(() => {
    if (cffs.stat(pathOrFd as URL, { followSymlinks: false }) == null) {
      throw new ERR_ENOENT((path as URL).pathname, { syscall: 'lchown' });
    }
  }, callback);
}

export function lutimes(
  path: FilePath,
  atime: RawTime | Date,
  mtime: RawTime | Date,
  callback: ErrorOnlyCallback
): void {
  atime = getDate(atime);
  mtime = getDate(mtime);
  path = normalizePath(path);
  callWithErrorOnlyCallback(() => {
    fssync.lutimesSync(path, atime, mtime);
  }, callback);
}

export function link(
  src: FilePath,
  dest: FilePath,
  callback: ErrorOnlyCallback
): void {
  const normalizedSrc = normalizePath(src);
  const normalizedDest = normalizePath(dest);
  callWithErrorOnlyCallback(() => {
    fssync.linkSync(normalizedSrc, normalizedDest);
  }, callback);
}

export function lstat(
  path: FilePath,
  optionsOrCallback: SingleArgCallback<Stats | undefined> | StatOptions,
  callback?: SingleArgCallback<Stats | undefined>
): void {
  let options: StatOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = { bigint: false };
  } else {
    options = optionsOrCallback;
  }
  validateStatArgs(path, options);
  callWithSingleArgCallback(() => fssync.lstatSync(path, options), callback);
}

export function mkdir(
  path: FilePath,
  optionsOrCallback:
    | number
    | SingleArgCallback<string | undefined>
    | MakeDirectoryOptions,
  callback?: SingleArgCallback<string | undefined>
): void {
  let options: number | MakeDirectoryOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }
  const { path: normalizedPath, recursive } = validateMkDirArgs(path, options);
  callWithSingleArgCallback(
    () => fssync.mkdirSync(normalizedPath, { recursive }),
    callback
  );
}

export function mkdtemp(
  prefix: FilePath,
  optionsOrCallback: SingleArgCallback<string> | MkdirTempSyncOptions,
  callback?: SingleArgCallback<string>
): void {
  let options: MkdirTempSyncOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }
  callWithSingleArgCallback(
    () => fssync.mkdtempSync(prefix, options),
    callback
  );
}

export function open(
  path: FilePath,
  flagsOrCallback: string | number | SingleArgCallback<number> = 'r',
  modeOrCallback: string | number | SingleArgCallback<number> = 0o666,
  callback?: SingleArgCallback<number>
): void {
  let flags: string | number;
  let mode: string | number;
  if (typeof flagsOrCallback === 'function') {
    callback = flagsOrCallback;
    flags = 'r';
    mode = 0o666;
  } else if (typeof modeOrCallback === 'function') {
    callback = modeOrCallback;
    flags = flagsOrCallback;
    mode = 0o666;
  } else {
    flags = flagsOrCallback;
    mode = modeOrCallback;
  }
  path = normalizePath(path);
  mode = parseFileMode(mode, 'mode');
  flags = stringToFlags(flags);
  callWithSingleArgCallback(() => fssync.openSync(path, flags, mode), callback);
}

export function opendir(
  path: FilePath,
  optionsOrCallback: SingleArgCallback<Dir> | OpenDirOptions,
  callback?: SingleArgCallback<Dir>
): void {
  let options: OpenDirOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {
      encoding: 'utf8',
      bufferSize: 32,
      recursive: false,
    };
  } else {
    options = optionsOrCallback;
  }

  const {
    path: validatedPath,
    encoding,
    recursive,
  } = validateOpendirArgs(path, options);

  callWithSingleArgCallback(() => {
    return fssync.opendirSync(validatedPath, {
      encoding: encoding as BufferEncoding,
      recursive,
    });
  }, callback);
}

// read has a complex polymorphic signature so this is a bit gnarly.
// The various signatures include:
//   fs.read(fd, buffer, offset, length, position, callback)
//   fs.read(fd, callback)
//   fs.read(fd, buffer, callback)
//   fs.read(fd, buffer, { offset, length, position }, callback)
//   fs.read(fd, { buffer, offset, length, position }, callback)
//
// Where fd is always a number, buffer is an ArrayBufferView, offset and
// length are numbers, but position can be a number or bigint, and offset
// length, and position are optional. The callback is always a function
// that receives three arguments: err, bytesRead, and buffer.
export function read<T extends NodeJS.ArrayBufferView>(
  fd: number,
  bufferOptionsOrCallback:
    | T
    | ReadAsyncOptions<T>
    | DoubleArgCallback<number, T>,
  offsetOptionsOrCallback?:
    | ReadAsyncOptions<T>
    | number
    | DoubleArgCallback<number, T>,
  lengthOrCallback?: null | number | DoubleArgCallback<number, T>,
  position?: Position,
  callback?: DoubleArgCallback<number, T>
): void {
  // Node.js... you're killing me here with these polymorphic signatures.
  //
  // We're going to normalize the arguments so that we can defer to the
  // readSync variant using the signature readSync(fd, buffer, options)
  //
  // The callback is always the last argument but may appear in the second,
  // third, fourth, or sixth position depending on the signature used. When we
  // find it, we can ignore the remaining arguments that come after it,
  // defaulting any missing arguments to whatever default is defined for
  // them.
  //
  // The second argument is always either a buffer, an options object that
  // contains a buffer property, or the callback. If it's the callback,
  // then we will allocate a new buffer for the read with size 16384 bytes,
  // and default the offset to 0, length to the buffer size, and position to
  // null (indicating that the internal read position for the fd is to be
  // used). If it's an options object, we will use the buffer property from it
  // if it exists. If the buffer property is not present, we will allocate a
  // new buffer for the read with size 16384 bytes. The offset, length, and
  // position properties will be used if they are present in the options
  // object or defaulted to 0, the buffer size, and null respectively.
  // If the second argument is a buffer, we will use it and look for the
  // offset, length, position, and callback arguments in the remaining arguments.
  //
  // The third argument is either ignored (if the second argument is the
  // callback), or it is one of either the offset, the options object, or
  // the callback. If it is the callback, we will default the offset to 0,
  // length to the buffer size (that had to have been provided by the second
  // argument), and position to null. If it is the options object, we will
  // get the offset, length, and position properties from it if they exist,
  // or default them to 0, the buffer size, and null respectively. If it is
  // the offset, we will look for the length, position, and callback in the
  // remaining arguments.
  //
  // The fourth argument is either ignored (if the second or third argument is
  // the callback), or it is the length as either a number, null, or explicitly
  // passed as undefined, or it is the callback. If it is the callback, we will
  // default the length to the buffer size (that had to have been provided by
  // the second argument), and default position to null, then look for the
  // callback in the sixth argument. If it is the length, we will look for the
  // position and callback in the remaining arguments.
  //
  // The fifth argument is either ignored (if the callback has already been
  // seen) or it is the position as either a number, bigint, null, or explicitly
  // undefined. Any other type in this position is an error.
  //
  // The sixth argument is either ignored (if the callback has already been
  // seen) or it is the callback. If it is not a function then an error is
  // thrown.
  //
  // Once we have collected all of the arguments, we will call the readSync
  // method with signature readSync(fd, buffer, { offset, length, position })
  // and pass the return value, and the buffer, to the Node.js-style callback
  // with the signature callback(null, returnValue, buffer). If the call throws,
  // then we will pass the error to the callback as the first argument.

  let actualCallback: undefined | DoubleArgCallback<number, T>;
  let actualBuffer: T; // Buffer, TypedArray, or DataView
  let actualOffset = 0; // Offset from the beginning of the buffer
  let actualLength: number; // Length of the data to read into the buffer
  // Should never be negative and never extends
  // beyond the end of the buffer (that is,
  // actualOffset + actualLength <= actualBuffer.byteLength)
  let actualPosition: Position = null; // The position within the
  // file to read from. If null,
  // the current position for the fd
  // is used.

  // Handle the case where the second argument is the callback
  if (typeof bufferOptionsOrCallback === 'function') {
    actualCallback = bufferOptionsOrCallback;
    // Default buffer size when not provided
    // The use of as unknown as T here is a bit of a hack to satisfy the types...
    actualBuffer = Buffer.alloc(16384) as unknown as T;
    actualLength = actualBuffer.byteLength;
  }
  // Handle the case where the second argument is an options object
  else if (
    // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
    bufferOptionsOrCallback != null &&
    typeof bufferOptionsOrCallback === 'object' &&
    !isArrayBufferView(bufferOptionsOrCallback)
  ) {
    // It's an options object
    const {
      buffer = Buffer.alloc(16384),
      offset = buffer.byteOffset,
      length = buffer.byteLength,
      position = null,
    } = bufferOptionsOrCallback;
    if (!isArrayBufferView(buffer)) {
      throw new ERR_INVALID_ARG_TYPE(
        'options.buffer',
        ['Buffer', 'TypedArray', 'DataView'],
        buffer
      );
    }
    validateUint32(offset, 'options.offset');
    validateUint32(length, 'options.length');
    validatePosition(position, 'options.position');

    actualBuffer = buffer as unknown as T;
    actualOffset = offset;
    actualLength = length;
    actualPosition = position;

    // The callback must be in the third argument
    if (typeof offsetOptionsOrCallback !== 'function') {
      throw new ERR_INVALID_ARG_TYPE(
        'callback',
        ['function'],
        offsetOptionsOrCallback
      );
    }
    actualCallback = offsetOptionsOrCallback;
  }
  // Handle the case where the second argument is a buffer
  else {
    actualBuffer = bufferOptionsOrCallback;

    if (!isArrayBufferView(actualBuffer)) {
      throw new ERR_INVALID_ARG_TYPE(
        'buffer',
        ['Buffer', 'TypedArray', 'DataView'],
        actualBuffer
      );
    }

    actualLength = actualBuffer.byteLength;
    actualOffset = actualBuffer.byteOffset;

    // Now we need to find the callback and other parameters
    if (typeof offsetOptionsOrCallback === 'function') {
      // fs.read(fd, buffer, callback)
      actualCallback = offsetOptionsOrCallback;
    } else if (
      typeof offsetOptionsOrCallback === 'object' &&
      !(offsetOptionsOrCallback instanceof Number)
    ) {
      // fs.read(fd, buffer, options, callback)
      const {
        offset = actualOffset,
        length = actualLength,
        position = null,
      } = offsetOptionsOrCallback;
      validateUint32(offset, 'options.offset');
      validateUint32(length, 'options.length');
      validatePosition(position, 'options.position');
      actualOffset = offset;
      actualLength = length;
      actualPosition = position;

      // The callback must be in the fourth argument.
      if (typeof lengthOrCallback !== 'function') {
        throw new ERR_INVALID_ARG_TYPE(
          'callback',
          ['function'],
          lengthOrCallback
        );
      }
      actualCallback = lengthOrCallback;
    } else {
      // fs.read(fd, buffer, offset, length, position, callback)
      actualOffset =
        typeof offsetOptionsOrCallback === 'number'
          ? offsetOptionsOrCallback
          : 0;

      if (typeof lengthOrCallback === 'function') {
        actualCallback = lengthOrCallback;
        actualPosition = null;
        actualLength = actualBuffer.byteLength;
      } else {
        actualLength = lengthOrCallback ?? actualBuffer.byteLength;

        validateUint32(position, 'position');
        actualPosition = position;

        actualCallback = callback;
      }
    }
  }

  // We know that the function must be called with at least 3 arguments and
  // that the first argument is always a number (the fd) and the last must
  // always be the callback.
  // If the actualCallback is not set at this point, then we have a problem.
  if (typeof actualCallback !== 'function') {
    throw new ERR_INVALID_ARG_TYPE('callback', ['function'], actualCallback);
  }

  // We also have a problem if the actualBuffer is not set here correctly.
  if (!isArrayBufferView(actualBuffer)) {
    throw new ERR_INVALID_ARG_TYPE(
      'buffer',
      ['Buffer', 'TypedArray', 'DataView'],
      actualBuffer
    );
  }

  // At this point we have the following:
  // - actualBuffer: The buffer to read into
  // - actualOffset: The offset into the buffer to start writing at
  // - actualLength: The length of the data to read into the buffer
  // - actualPosition: The position within the file to read from (or null)
  // - actualCallback: The callback to call when done
  // - fd: The file descriptor to read from
  // Let actualOffset + actualLength should never be greater than the
  // buffer size. Let's check that.
  if (
    actualOffset < 0 ||
    actualLength < 0 ||
    actualOffset + actualLength > actualBuffer.byteLength
  ) {
    throw new ERR_INVALID_ARG_VALUE(
      'offset',
      'must be >= 0 and <= buffer.length'
    );
  }
  // The actualOffset, actualLength, and actualPosition values should always
  // be greater or equal to 0 (unless actualPosition is null)... keeping in
  // mind that actualPosition can be a number or a bigint.

  // As a special case, if the actualBuffer length is 0, or if actualLength
  // is 0, then can just call the callback with 0 bytes read and return.
  if (actualBuffer.byteLength === 0 || actualLength === 0) {
    queueMicrotask(() => {
      actualCallback(null, 0, actualBuffer);
    });
    return;
  }

  // Now that we've normalized all the parameters, call readSync
  try {
    const bytesRead = fssync.readSync(fd, actualBuffer, {
      offset: actualOffset,
      length: actualLength,
      position: actualPosition,
    });
    queueMicrotask(() => {
      actualCallback(null, bytesRead, actualBuffer);
    });
  } catch (err) {
    queueMicrotask(() => {
      actualCallback(err);
    });
  }
}

export function readdir(
  path: FilePath,
  optionsOrCallback: SingleArgCallback<ReadDirResult> | ReadDirOptions,
  callback?: SingleArgCallback<ReadDirResult>
): void {
  let options: ReadDirOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {
      encoding: 'utf8',
      withFileTypes: false,
      recursive: false,
    };
  } else {
    options = optionsOrCallback;
  }
  const {
    path: normalizedPath,
    recursive,
    withFileTypes,
    encoding,
  } = validateReaddirArgs(path, options);
  callWithSingleArgCallback(() => {
    return fssync.readdirSync(normalizedPath, {
      recursive,
      withFileTypes,
      encoding,
    });
  }, callback);
}

export function readFile(
  path: FilePath,
  optionsOrCallback:
    | SingleArgCallback<string | Buffer>
    | BufferEncoding
    | null
    | ReadFileSyncOptions,
  callback?: SingleArgCallback<string | Buffer>
): void {
  let options: BufferEncoding | null | ReadFileSyncOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }
  // TODO(node-fs): Validate options more. Specifically the encoding option
  callWithSingleArgCallback(() => fssync.readFileSync(path, options), callback);
}

export function readlink(
  path: FilePath,
  optionsOrCallback:
    | SingleArgCallback<string | Buffer>
    | BufferEncoding
    | null
    | ReadLinkSyncOptions,
  callback?: SingleArgCallback<string | Buffer>
): void {
  let options: BufferEncoding | 'buffer' | null | ReadLinkSyncOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }
  path = normalizePath(path);
  validateObject(options, 'options');
  const { encoding = 'utf8' } = options;
  if (encoding !== 'buffer' && !Buffer.isEncoding(encoding)) {
    throw new ERR_INVALID_ARG_VALUE('options.encoding', encoding);
  }
  callWithSingleArgCallback(
    () => fssync.readlinkSync(path, { encoding }),
    callback
  );
}

export function readv<T extends NodeJS.ArrayBufferView>(
  fd: number,
  buffers: T[],
  positionOrCallback: undefined | Position | DoubleArgCallback<number, T[]>,
  callback?: DoubleArgCallback<number, T[]>
): void {
  if (typeof positionOrCallback === 'function') {
    callback = positionOrCallback;
    positionOrCallback = null;
  }
  if (typeof callback !== 'function') {
    throw new ERR_INVALID_ARG_TYPE('callback', ['function'], callback);
  }

  validatePosition(positionOrCallback, 'position');

  try {
    const read = fssync.readvSync(fd, buffers, positionOrCallback);
    queueMicrotask(() => {
      callback(null, read, buffers);
    });
  } catch (err) {
    queueMicrotask(() => {
      callback(err);
    });
  }
}

export function realpath(
  path: FilePath,
  optionsOrCallback:
    | SingleArgCallback<string | Buffer>
    | BufferEncoding
    | null
    | ReadLinkSyncOptions,
  callback?: SingleArgCallback<string | Buffer>
): void {
  let options: BufferEncoding | null | ReadLinkSyncOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }

  validateObject(options, 'options');
  const { encoding = 'utf8' } = options;
  if (encoding !== 'buffer' && !Buffer.isEncoding(encoding)) {
    throw new ERR_INVALID_ARG_VALUE('options.encoding', encoding);
  }

  callWithSingleArgCallback(
    () => fssync.realpathSync(path, { encoding }),
    callback
  );
}

realpath.native = realpath;

export function rename(
  oldPath: FilePath,
  newPath: FilePath,
  callback: ErrorOnlyCallback
): void {
  const normalizedOldPath = normalizePath(oldPath);
  const normalizedNewPath = normalizePath(newPath);
  callWithErrorOnlyCallback(() => {
    fssync.renameSync(normalizedOldPath, normalizedNewPath);
  }, callback);
}

export function rmdir(
  path: FilePath,
  optionsOrCallback: ErrorOnlyCallback | RmDirOptions,
  callback?: ErrorOnlyCallback
): void {
  let options: RmDirOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }
  const { path: normalizedPath, recursive } = validateRmDirArgs(path, options);
  callWithErrorOnlyCallback(() => {
    fssync.rmdirSync(normalizedPath, { recursive });
  }, callback);
}

export function rm(
  path: FilePath,
  optionsOrCallback: ErrorOnlyCallback | RmOptions,
  callback?: ErrorOnlyCallback
): void {
  let options: RmOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }
  const {
    path: normalizedPath,
    recursive,
    force,
  } = validateRmArgs(path, options);
  callWithErrorOnlyCallback(() => {
    fssync.rmSync(normalizedPath, { recursive, force });
  }, callback);
}

export function stat(
  path: FilePath,
  optionsOrCallback: SingleArgCallback<Stats | undefined> | StatOptions,
  callback?: SingleArgCallback<Stats | undefined>
): void {
  let options: StatOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = { bigint: false };
  } else {
    options = optionsOrCallback;
  }
  validateStatArgs(path, options);
  callWithSingleArgCallback(() => fssync.statSync(path, options), callback);
}

export function statfs(
  path: FilePath,
  optionsOrCallback:
    | SingleArgCallback<StatsFs | BigIntStatsFs>
    | { bigint?: boolean | undefined },
  callback?: SingleArgCallback<StatsFs | BigIntStatsFs>
): void {
  let options: { bigint?: boolean | undefined };
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = { bigint: false };
  } else {
    options = optionsOrCallback;
  }
  callWithSingleArgCallback(() => fssync.statfsSync(path, options), callback);
}

export function symlink(
  target: FilePath,
  path: FilePath,
  typeOrCallback: SymlinkType | ErrorOnlyCallback,
  callback?: ErrorOnlyCallback
): void {
  let type: SymlinkType;
  if (typeof typeOrCallback === 'function') {
    callback = typeOrCallback;
    type = null;
  } else {
    type = typeOrCallback;
  }
  const normalizedTarget = normalizePath(target);
  const normalizedPath = normalizePath(path);
  if (type != null) {
    validateOneOf(type, 'type', ['dir', 'file', 'junction', null]);
  }
  callWithErrorOnlyCallback(() => {
    fssync.symlinkSync(normalizedTarget, normalizedPath, type);
  }, callback);
}

export function truncate(
  path: FilePath,
  lenOrCallback: number | ErrorOnlyCallback,
  callback?: ErrorOnlyCallback
): void {
  let len: number;
  if (typeof lenOrCallback === 'function') {
    callback = lenOrCallback;
    len = 0;
  } else {
    len = lenOrCallback;
  }
  validateUint32(len, 'len');
  callWithErrorOnlyCallback(() => {
    fssync.truncateSync(path, len);
  }, callback);
}

export function unlink(path: FilePath, callback: ErrorOnlyCallback): void {
  const normalizedPath = normalizePath(path);
  callWithErrorOnlyCallback(() => {
    fssync.unlinkSync(normalizedPath);
  }, callback);
}

export function utimes(
  path: FilePath,
  atime: RawTime | Date,
  mtime: RawTime | Date,
  callback: ErrorOnlyCallback
): void {
  atime = getDate(atime);
  mtime = getDate(mtime);
  path = normalizePath(path);
  callWithErrorOnlyCallback(() => {
    fssync.utimesSync(path, atime, mtime);
  }, callback);
}

export function write<T extends NodeJS.ArrayBufferView>(
  fd: number,
  buffer: T | string,
  offsetOptionsPositionOrCallback?:
    | WriteSyncOptions
    | Position
    | DoubleArgCallback<number, T>,
  encodingLengthOrCallback?:
    | number
    | BufferEncoding
    | DoubleArgCallback<number, T>,
  positionOrCallback?: Position | DoubleArgCallback<number, T>,
  callback?: DoubleArgCallback<number, T>
): void {
  let offsetOrOptions: WriteSyncOptions | Position | undefined;
  let lengthOrEncoding: number | BufferEncoding | null | undefined;
  let position: Position | undefined;
  if (typeof offsetOptionsPositionOrCallback === 'function') {
    callback = offsetOptionsPositionOrCallback;
    offsetOrOptions = undefined;
  } else {
    offsetOrOptions = offsetOptionsPositionOrCallback;
  }
  if (typeof encodingLengthOrCallback === 'function') {
    callback = encodingLengthOrCallback;
    lengthOrEncoding = undefined;
  } else {
    lengthOrEncoding = encodingLengthOrCallback;
  }
  if (typeof positionOrCallback === 'function') {
    callback = positionOrCallback;
    position = undefined;
  } else {
    position = positionOrCallback;
  }
  if (typeof callback !== 'function') {
    throw new ERR_INVALID_ARG_TYPE('callback', ['function'], callback);
  }
  // Because the callback expects the buffer to be returned in the callback,
  // we need to make sure that the buffer is not a string here rather than
  // relying on the transformation in the writeSync call.
  if (typeof buffer === 'string') {
    let encoding = 'utf8';
    if (typeof lengthOrEncoding === 'string') {
      encoding = lengthOrEncoding;
      lengthOrEncoding = undefined;
    }
    buffer = Buffer.from(buffer, encoding) as unknown as T;
  }

  const {
    fd: validatedFd,
    buffer: actualBuffer,
    position: actualPosition,
  } = validateWriteArgs(
    fd,
    buffer,
    offsetOrOptions,
    lengthOrEncoding,
    position
  );

  try {
    const written = fssync.writevSync(
      validatedFd,
      actualBuffer,
      actualPosition
    );
    queueMicrotask(() => {
      callback(null, written, buffer as unknown as T);
    });
  } catch (err) {
    queueMicrotask(() => {
      callback(err);
    });
  }
}

export function writeFile(
  path: number | FilePath,
  data: string | ArrayBufferView,
  optionsOrCallback:
    | ErrorOnlyCallback
    | BufferEncoding
    | null
    | WriteFileOptions,
  callback?: ErrorOnlyCallback
): void {
  let options: BufferEncoding | null | WriteFileOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {
      encoding: 'utf8',
      mode: 0o666,
      flag: 'w',
      flush: false,
    };
  } else {
    options = optionsOrCallback;
  }

  const {
    path: validatedPath,
    data: validatedData,
    append,
    exclusive,
  } = validateWriteFileArgs(path, data, options);

  callWithSingleArgCallback<number>(
    () => cffs.writeAll(validatedPath, validatedData, { append, exclusive }),
    callback
  );
}

export function writev<T extends NodeJS.ArrayBufferView>(
  fd: number,
  buffers: T[],
  positionOrCallback?: Position | DoubleArgCallback<number, T[]>,
  callback?: DoubleArgCallback<number, T[]>
): void {
  if (typeof positionOrCallback === 'function') {
    callback = positionOrCallback;
    positionOrCallback = null;
  }
  if (typeof callback !== 'function') {
    throw new ERR_INVALID_ARG_TYPE('callback', ['function'], callback);
  }

  fd = getValidatedFd(fd);
  validateBufferArray(buffers);
  validatePosition(positionOrCallback, 'position');

  try {
    const written = fssync.writevSync(fd, buffers, positionOrCallback);
    queueMicrotask(() => {
      callback(null, written, buffers);
    });
  } catch (err) {
    queueMicrotask(() => {
      callback(err);
    });
  }
}

export function unwatchFile(): void {
  // We currently do not implement file watching.
  throw new Error('Not implemented');
}

export function watch(): void {
  // We currently do not implement file watching.
  throw new Error('Not implemented');
}

export function watchFile(): void {
  // We currently do not implement file watching.
  throw new Error('Not implemented');
}

export function glob(
  _pattern: string | readonly string[],
  _options:
    | GlobOptions
    | GlobOptionsWithFileTypes
    | GlobOptionsWithoutFileTypes,
  _callback: ErrorOnlyCallback
): void {
  // We do not yet implement the globSync function. In Node.js, this
  // function depends heavily on the third party minimatch library
  // which is not yet available in the workers runtime. This will be
  // explored for implementation separately in the future.
  throw new ERR_UNSUPPORTED_OPERATION();
}

// An API is considered stubbed if it is not implemented by the function
// exists with the correct signature and throws an error if called. If
// a function exists that does not have the correct signature, it is
// not considered fully stubbed.
// An API is considered optimized if the API has been implemented and
// tested and then optimized for performance.
// Implemented APIs here are a bit different than in the sync version
// since most of these are implemented in terms of calling the sync
// version. We consider it implemented here if the code is present and
// calls the sync api even if the sync api itself it not fully implemented.
//
// (S == Stubbed, I == Implemented, T == Tested, O == Optimized)
//  S  I  T  O
// [x][x][x][x] fs.access(path[, mode], callback)
// [x][x][x][x] fs.chmod(path, mode, callback)
// [x][x][x][x] fs.chown(path, uid, gid, callback)
// [x][x][x][x] fs.exists(path, callback)
// [x][x][x][x] fs.fchmod(fd, mode, callback)
// [x][x][x][x] fs.fchown(fd, uid, gid, callback)
// [x][x][x][x] fs.futimes(fd, atime, mtime, callback)
// [x][x][x][x] fs.lchmod(path, mode, callback)
// [x][x][x][x] fs.lchown(path, uid, gid, callback)
// [x][x][x][x] fs.lutimes(path, atime, mtime, callback)
// [x][x][x][x] fs.utimes(path, atime, mtime, callback)
// [x][x][x][x] fs.fstat(fd[, options], callback)
// [x][x][x][x] fs.lstat(path[, options], callback)
// [x][x][x][x] fs.stat(path[, options], callback)
// [x][x][x][x] fs.statfs(path[, options], callback)
// [x][x][x][x] fs.fdatasync(fd, callback)
// [x][x][x][x] fs.fsync(fd, callback)
// [x][x][x][x] fs.link(existingPath, newPath, callback)
// [x][x][x][x] fs.readlink(path[, options], callback)
// [x][x][x][x] fs.realpath(path[, options], callback)
// [x][x][x][x] fs.realpath.native(path[, options], callback)
// [x][x][x][x] fs.symlink(target, path[, type], callback)
// [x][x][x][x] fs.unlink(path, callback)
// [x][x][x][x] fs.mkdir(path[, options], callback)
// [x][x][x][x] fs.mkdtemp(prefix[, options], callback)
// [x][x][x][x] fs.readdir(path[, options], callback)
// [x][x][x][x] fs.rmdir(path[, options], callback)
// [x][x][x][x] fs.rm(path[, options], callback)
// [x][x][x][x] fs.appendFile(path, data[, options], callback)
// [x][x][x][x] fs.close(fd[, callback])
// [x][x][x][x] fs.copyFile(src, dest[, mode], callback)
// [x][x][x][x] fs.ftruncate(fd[, len], callback)
// [x][x][x][x] fs.open(path[, flags[, mode]], callback)
// [x][x][x][x] fs.read(fd, buffer, offset, length, position, callback)
// [x][x][x][x] fs.read(fd[, options], callback)
// [x][x][x][x] fs.read(fd, buffer[, options], callback)
// [x][x][x][x] fs.readFile(path[, options], callback)
// [x][x][x][x] fs.readv(fd, buffers[, position], callback)
// [x][x][x][x] fs.rename(oldPath, newPath, callback)
// [x][x][x][x] fs.truncate(path[, len], callback)
// [x][x][x][x] fs.write(fd, buffer, offset[, length[, position]], callback)
// [x][x][x][x] fs.write(fd, buffer[, options], callback)
// [x][x][x][x] fs.write(fd, string[, position[, encoding]], callback)
// [x][x][x][x] fs.writeFile(file, data[, options], callback)
// [x][x][x][x] fs.writev(fd, buffers[, position], callback)//
// [x][x][x][x] fs.opendir(path[, options], callback)
// [-][-][-][-] fs.unwatchFile(filename[, listener])
// [-][-][-][-] fs.watch(filename[, options][, listener])
// [-][-][-][-] fs.watchFile(filename[, options], listener)
// [x][x][x][x] fs.cp(src, dest[, options], callback)
//
// [ ][ ][ ][ ] fs.createReadStream(path[, options])
// [ ][ ][ ][ ] fs.createWriteStream(path[, options])
//
// [ ][ ][ ][ ] fs.glob(pattern[, options], callback)
// [ ][ ][ ][ ] fs.openAsBlob(path[, options])
