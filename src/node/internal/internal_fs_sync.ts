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
/* eslint-disable @typescript-eslint/no-unused-vars,@typescript-eslint/no-unnecessary-condition */

import {
  stringToFlags,
  getValidatedFd,
  validateBufferArray,
  validateStringAfterArrayBufferView,
  normalizePath,
  getDate,
  Stats,
  kBadge,
  type FilePath,
  type Position,
  type RawTime,
  type SymlinkType,
  validatePosition,
  validateAccessArgs,
  validateChownArgs,
  validateChmodArgs,
  validateStatArgs,
} from 'node-internal:internal_fs_utils';
import {
  validateInteger,
  parseFileMode,
  validateBoolean,
  validateObject,
  validateOneOf,
  validateEncoding,
  validateUint32,
} from 'node-internal:validators';
import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_ENOENT,
  ERR_EBADF,
  ERR_EINVAL,
} from 'node-internal:internal_errors';
import { isArrayBufferView } from 'node-internal:internal_types';
import {
  F_OK,
  X_OK,
  W_OK,
  O_WRONLY,
  O_RDWR,
  O_APPEND,
  O_EXCL,
  COPYFILE_EXCL,
  COPYFILE_FICLONE_FORCE,
} from 'node-internal:internal_fs_constants';
import { type Dir, Dirent } from 'node-internal:internal_fs';
import { default as cffs } from 'cloudflare-internal:filesystem';

import { Buffer } from 'node-internal:internal_buffer';
import type {
  BigIntStatsFs,
  CopySyncOptions,
  MakeDirectoryOptions,
  OpenDirOptions,
  ReadSyncOptions,
  RmOptions,
  RmDirOptions,
  StatsFs,
  WriteFileOptions,
} from 'node:fs';

export function accessSyncImpl(
  path: URL,
  mode: number,
  followSymlinks: boolean
): void {
  // Input validation should have already been done by the caller.

  // If the X_OK flag is set we will always throw because we don't
  // support executable files.
  if (mode & X_OK) {
    throw new ERR_ENOENT(path.pathname, { syscall: 'access' });
  }

  const stat = cffs.stat(path, { followSymlinks });

  // Similar to node.js, we make no differentiation between the file
  // not existing and the file existing but not being accessible.
  if (stat == null || (mode & W_OK && !stat.writable)) {
    // Not found... or not writable
    throw new ERR_ENOENT(path.pathname, { syscall: 'access' });
  }

  // We always assume that files are readable, so if we get here the
  // path is accessible.
}

export function accessSync(path: FilePath, mode: number = F_OK): void {
  const { path: actualPath, mode: actualMode } = validateAccessArgs(path, mode);
  accessSyncImpl(actualPath, actualMode, true);
}

export function appendFileSync(
  path: number | FilePath,
  data: string | ArrayBufferView,
  options: BufferEncoding | null | WriteFileOptions = {}
): number {
  if (typeof options === 'string' || options == null) {
    options = { encoding: options as BufferEncoding | null };
  }
  const {
    encoding = 'utf8',
    mode = 0o666,
    flag = 'a',
    flush = false,
  } = options ?? {};
  // We defer to writeFileSync for the actual implementation and validation
  return writeFileSync(path, data, { encoding, mode, flag, flush });
}

export function chmodSync(path: FilePath, mode: string | number): void {
  const { pathOrFd } = validateChmodArgs(path, mode);
  if (cffs.stat(pathOrFd as URL, { followSymlinks: true }) == null) {
    throw new ERR_ENOENT((pathOrFd as URL).pathname, { syscall: 'chmod' });
  }
  // We do not implement chmod in any meaningful way as our filesystem
  // has no concept of user-defined permissions. Once we validate the inputs
  // we just return as a non-op.
  // The reason we call accessSync is to ensure, at the very least, that
  // the path exists and would otherwise be accessible.
}

export function chownSync(path: FilePath, uid: number, gid: number): void {
  const { pathOrFd } = validateChownArgs(path, uid, gid);
  if (cffs.stat(pathOrFd as URL, { followSymlinks: true }) == null) {
    throw new ERR_ENOENT((pathOrFd as URL).pathname, { syscall: 'chown' });
  }
  // We do not implement chown in any meaningful way as our filesystem
  // has no concept of ownership. Once we validate the inputs we just
  // return as a non-op.
  // The reason we call accessSync is to ensure, at the very least, that
  // the path exists and would otherwise be accessible.
}

export function closeSync(fd: number): void {
  cffs.close(getValidatedFd(fd));
}

export function copyFileSync(
  src: FilePath,
  dest: FilePath,
  _mode: number
): void {
  cffs.renameOrCopy(normalizePath(src), normalizePath(dest), { copy: true });
}

export function cpSync(
  src: FilePath,
  dest: FilePath,
  options: CopySyncOptions = {}
): void {
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

  if (filter !== undefined && typeof filter !== 'function') {
    throw new ERR_INVALID_ARG_TYPE('options.filter', 'function', filter);
  }

  const exclusive = Boolean(mode & COPYFILE_EXCL);
  // We're not current implementing the exclusive flag. We're validating
  // it here just to use it so the compiler doesn't complain.
  validateBoolean(exclusive, '');

  src = normalizePath(src);
  dest = normalizePath(dest);
  throw new Error('Not implemented');
}

export function existsSync(path: FilePath): boolean {
  try {
    // The existsSync function follows symlinks. If the symlink is broken,
    // it will return false.
    accessSync(path);
    return true;
  } catch {
    // It's always odd to swallow errors but this is how Node.js does it.
    // The existsSync function never throws and returns false if any error
    // occurs.
    return false;
  }
}

export function fchmodSync(fd: number, mode: string | number): void {
  fd = getValidatedFd(fd);
  parseFileMode(mode, 'mode');
  if (cffs.stat(fd, { followSymlinks: true }) == null) {
    throw new ERR_EBADF({ syscall: 'fchmod' });
  }
  // We do not implement chmod in any meaningful way as our filesystem
  // has no concept of user-defined permissions. Once we validate the inputs
  // we just return as a non-op.
  // The reason we call cffs.stat is to ensure, at the very least, that
  // the fd is valid and would otherwise be accessible.
}

export function fchownSync(fd: number, uid: number, gid: number): void {
  const { pathOrFd } = validateChownArgs(fd, uid, gid);
  if (cffs.stat(pathOrFd as number, { followSymlinks: true }) == null) {
    throw new ERR_EBADF({ syscall: 'fchown' });
  }
  // We do not implement chown in any meaningful way as our filesystem
  // has no concept of ownership. Once we validate the inputs we just
  // return as a non-op.
  // The reason we call accessSync is to ensure, at the very least, that
  // the path exists and would otherwise be accessible.
}

export function fdatasyncSync(fd: number): void {
  fd = getValidatedFd(fd);
  // We do not implement fdatasync in any meaningful way. At most we
  // will validate that the fd is valid and would otherwise be accessible.
  if (cffs.stat(fd, { followSymlinks: true }) == null) {
    throw new ERR_EBADF({ syscall: 'datasync' });
  }
}

export type FStatOptions = {
  bigint?: boolean | undefined;
};

export function fstatSync(fd: number, options: FStatOptions = {}): Stats {
  validateObject(options, 'options');
  const { pathOrFd: validatedFd, bigint } = validateStatArgs(
    fd,
    options,
    true /* is fstat */
  );
  const stat = cffs.stat(validatedFd as number, { followSymlinks: true });
  if (stat == null) {
    throw new ERR_EBADF({ syscall: 'stat' });
  }
  return new Stats(kBadge, stat, { bigint });
}

export function fsyncSync(fd: number): void {
  fd = getValidatedFd(fd);
  // We do not implement fdatasync in any meaningful way. At most we
  // will validate that the fd is valid and would otherwise be accessible.
  if (cffs.stat(fd, { followSymlinks: true }) == null) {
    throw new ERR_EBADF({ syscall: 'sync' });
  }
}

export function ftruncateSync(fd: number, len: number = 0): void {
  validateUint32(len, 'len');
  cffs.truncate(getValidatedFd(fd), len);
}

export function futimesSync(
  fd: number,
  atime: RawTime | Date,
  mtime: RawTime | Date
): void {
  // We do not actually make use of access time in our filesystem. We just
  // validate the inputs here.
  atime = getDate(atime);
  mtime = getDate(mtime);
  cffs.setLastModified(getValidatedFd(fd), mtime, {});
}

export function lchmodSync(path: FilePath, mode: string | number): void {
  const { pathOrFd } = validateChmodArgs(path, mode);
  if (cffs.stat(pathOrFd as URL, { followSymlinks: false }) == null) {
    throw new ERR_ENOENT((pathOrFd as URL).pathname, { syscall: 'lchmod' });
  }
  // We do not implement chmod in any meaningful way as our filesystem
  // has no concept of user-defined permissions. Once we validate the inputs
  // we just return as a non-op.
  // The reason we call cffs.stat is to ensure, at the very least, that
  // the fd is valid and would otherwise be accessible.
}

export function lchownSync(path: FilePath, uid: number, gid: number): void {
  const { pathOrFd } = validateChownArgs(path, uid, gid);
  if (cffs.stat(pathOrFd as URL, { followSymlinks: false }) == null) {
    throw new ERR_ENOENT((path as URL).pathname, { syscall: 'lchown' });
  }
  // We do not implement chown in any meaningful way as our filesystem
  // has no concept of user-defined permissions. Once we validate the inputs
  // we just return as a non-op.
  // The reason we call cffs.stat is to ensure, at the very least, that
  // the fd is valid and would otherwise be accessible.
}

export function lutimesSync(
  path: FilePath,
  atime: RawTime | Date,
  mtime: RawTime | Date
): void {
  // We do not actually make use of access time in our filesystem. We just
  // validate the inputs here.
  atime = getDate(atime);
  mtime = getDate(mtime);
  cffs.setLastModified(normalizePath(path), mtime, { followSymlinks: false });
}

export function linkSync(existingPath: FilePath, newPath: FilePath): void {
  cffs.link(normalizePath(newPath), normalizePath(existingPath), {
    symbolic: false,
  });
}

// We could use the StatSyncOptions from @types:node here but the definition
// of that in @types:node is bit overly complex for our use here.
export type StatOptions = {
  bigint?: boolean | undefined;
  throwIfNoEntry?: boolean | undefined;
};

export function lstatSync(
  path: FilePath,
  options: StatOptions = {}
): Stats | undefined {
  const {
    pathOrFd: validatedPath,
    bigint,
    throwIfNoEntry,
  } = validateStatArgs(path, options);
  const stat = cffs.stat(validatedPath as URL, { followSymlinks: false });
  if (stat == null) {
    if (throwIfNoEntry) {
      throw new ERR_ENOENT((validatedPath as URL).pathname, {
        syscall: 'lstat',
      });
    }
    return undefined;
  }
  return new Stats(kBadge, stat, { bigint });
}

export function mkdirSync(
  path: FilePath,
  options: number | MakeDirectoryOptions = {}
): string | undefined {
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

  return cffs.mkdir(normalizePath(path), { recursive, tmp: false });
}

export type MkdirTempSyncOptions = {
  encoding?: BufferEncoding | null | undefined;
};

export function mkdtempSync(
  prefix: FilePath,
  options: BufferEncoding | null | MkdirTempSyncOptions = {}
): string {
  if (typeof options === 'string' || options == null) {
    options = { encoding: options };
  }
  validateObject(options, 'options');
  const { encoding = 'utf8' } = options;
  validateEncoding(encoding, 'options.encoding');
  prefix = normalizePath(prefix, encoding);
  const ret = cffs.mkdir(normalizePath(prefix), {
    recursive: false,
    tmp: true,
  });
  if (ret === undefined) {
    // If mkdir failed it should throw a meaningful error. If we get
    // here, it means something else went wrong and we're just going
    // to throw a generic EINVAL error.
    throw new ERR_EINVAL({ syscall: 'mkdtemp' });
  }
  return ret;
}

export function opendirSync(path: FilePath, options: OpenDirOptions = {}): Dir {
  validateObject(options, 'options');
  const { encoding = 'utf8', bufferSize = 32, recursive = false } = options;
  validateEncoding(encoding, 'options.encoding');
  validateUint32(bufferSize, 'options.bufferSize');
  validateBoolean(recursive, 'options.recursive');
  path = normalizePath(path);
  // TODO(node-fs): Need implementation
  throw new Error('Not implemented');
}

export function openSync(
  path: FilePath,
  flags: string | number = 'r',
  mode: string | number = 0o666
): number {
  // We don't actually the the mode in any meaningful way. We just validate it.
  parseFileMode(mode, 'mode', 0o666);
  const newFlags = stringToFlags(flags);

  const read = !(newFlags & O_WRONLY) || Boolean(newFlags & O_RDWR);
  const write = Boolean(newFlags & O_WRONLY) || Boolean(newFlags & O_RDWR);
  const append = Boolean(newFlags & O_APPEND);
  const exclusive = Boolean(newFlags & O_EXCL);
  const followSymlinks = true;

  return cffs.open(normalizePath(path), {
    read,
    write,
    append,
    exclusive,
    followSymlinks,
  });
}

// We could use the @types/node definition here but it's a bit overly
// complex for our needs here.
export type ReadDirOptions = {
  encoding?: BufferEncoding | null | undefined;
  withFileTypes?: boolean | undefined;
  recursive?: boolean | undefined;
};

export type ReadDirResult = string[] | Buffer[] | Dirent[];

export function readdirSync(
  path: FilePath,
  options: BufferEncoding | null | ReadDirOptions = {}
): ReadDirResult {
  if (typeof options === 'string' || options == null) {
    options = { encoding: options };
  }
  validateObject(options, 'options');
  const {
    encoding = 'utf8',
    withFileTypes = false,
    recursive = false,
  } = options;
  validateEncoding(encoding, 'options.encoding');
  validateBoolean(withFileTypes, 'options.withFileTypes');
  validateBoolean(recursive, 'options.recursive');

  const handles = cffs.readdir(normalizePath(path), { recursive });

  if (withFileTypes) {
    return handles.map((handle) => {
      return new Dirent(handle.name, handle.type, handle.parentPath);
    });
  }

  return handles.map((handle) => {
    return handle.name;
  });
}

export type ReadFileSyncOptions = {
  encoding?: BufferEncoding | null | undefined;
  flag?: string | number | undefined;
};

export function readFileSync(
  pathOrFd: number | FilePath,
  options: BufferEncoding | null | ReadFileSyncOptions = {}
): string | Buffer {
  if (typeof options === 'string' || options == null) {
    options = { encoding: options };
  }
  validateObject(options, 'options');
  const { encoding = 'utf8', flag = 'r' } = options;
  validateEncoding(encoding, 'options.encoding');
  stringToFlags(flag);

  // TODO(node:fs): We are currently ignoring flags on readFileSync.

  const u8 = ((): Uint8Array => {
    if (typeof pathOrFd === 'number') {
      return cffs.readAll(getValidatedFd(pathOrFd));
    }
    return cffs.readAll(normalizePath(pathOrFd));
  })();

  const buf = Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength);
  if (typeof encoding === 'string') {
    return buf.toString(encoding);
  }
  return buf;
}

export type ReadLinkSyncOptions = {
  encoding?: BufferEncoding | null | undefined;
};

export function readlinkSync(
  path: FilePath,
  options: BufferEncoding | null | ReadLinkSyncOptions = {}
): string | Buffer {
  if (typeof options === 'string' || options == null) {
    options = { encoding: options };
  }
  validateObject(options, 'options');
  const { encoding = 'utf8' } = options;
  validateEncoding(encoding, 'options.encoding');
  const dest = Buffer.from(
    cffs.readLink(normalizePath(path), { failIfNotSymlink: true })
  );
  if (typeof encoding === 'string') {
    return dest.toString(encoding);
  }
  return dest;
}

// readSync is overloaded to support two different signatures:
//   fs.readSync(fd, buffer, offset, length, position)
//   fs.readSync(fd, buffer, options)
//
// fd is always a number, buffer is an ArrayBufferView, offset and length
// are numbers, and position is either a number, bigint, or null.
export function readSync(
  fd: number,
  buffer: NodeJS.ArrayBufferView,
  offsetOrOptions: ReadSyncOptions | number = {},
  length?: number,
  position: Position = null
): number {
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

  if (actualLength === 0 || buffer.byteLength === 0) {
    return 0;
  }

  return readvSync(
    fd,
    [Buffer.from(buffer.buffer, actualOffset, actualLength)],
    actualPosition
  );
}

export function readvSync(
  fd: number,
  buffers: NodeJS.ArrayBufferView[],
  position: Position = null
): number {
  fd = getValidatedFd(fd);
  validateBufferArray(buffers);
  validatePosition(position, 'position');

  if (buffers.length === 0) {
    return 0;
  }
  return cffs.read(fd, buffers, { position });
}

export function realpathSync(
  p: FilePath,
  options: BufferEncoding | null | ReadLinkSyncOptions = {}
): string | Buffer {
  if (typeof options === 'string' || options == null) {
    options = { encoding: options };
  }
  validateObject(options, 'options');
  const { encoding = 'utf8' } = options;
  validateEncoding(encoding, 'options.encoding');
  const dest = Buffer.from(
    cffs.readLink(normalizePath(p), { failIfNotSymlink: false })
  );
  if (typeof encoding === 'string') {
    return dest.toString(encoding);
  }
  return dest;
}

realpathSync.native = realpathSync;

export function renameSync(src: FilePath, dest: FilePath): void {
  cffs.renameOrCopy(normalizePath(src), normalizePath(dest), { copy: false });
}

export function rmdirSync(path: FilePath, options: RmDirOptions = {}): void {
  validateObject(options, 'options');
  const { maxRetries = 0, recursive = false, retryDelay = 0 } = options;
  // We do not implement the maxRetries or retryDelay options in any meaningful
  // way. We just validate them.
  validateUint32(maxRetries, 'options.maxRetries');
  validateBoolean(recursive, 'options.recursive');
  validateUint32(retryDelay, 'options.retryDelay');

  cffs.rm(normalizePath(path), { recursive, force: false, dironly: true });
}

export function rmSync(path: FilePath, options: RmOptions = {}): void {
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

  cffs.rm(normalizePath(path), { recursive, force, dironly: false });
}

export function statSync(
  path: FilePath,
  options: StatOptions = {}
): Stats | undefined {
  const {
    pathOrFd: validatedPath,
    bigint,
    throwIfNoEntry,
  } = validateStatArgs(path, options);
  const stat = cffs.stat(validatedPath as URL, { followSymlinks: true });
  if (stat == null) {
    if (throwIfNoEntry) {
      throw new ERR_ENOENT((validatedPath as URL).pathname, {
        syscall: 'stat',
      });
    }
    return undefined;
  }
  return new Stats(kBadge, stat, { bigint });
}

export function statfsSync(
  path: FilePath,
  options: { bigint?: boolean | undefined } = {}
): StatsFs | BigIntStatsFs {
  normalizePath(path);
  validateObject(options, 'options');
  const { bigint = false } = options;
  validateBoolean(bigint, 'options.bigint');
  // We don't implement statfs in any meaningful way. Nor will we actually
  // validate that the path exists. We just return a non-op dummy object.
  if (bigint) {
    return {
      type: 0n,
      bsize: 0n,
      blocks: 0n,
      bfree: 0n,
      bavail: 0n,
      files: 0n,
      ffree: 0n,
    };
  } else {
    return {
      type: 0,
      bsize: 0,
      blocks: 0,
      bfree: 0,
      bavail: 0,
      files: 0,
      ffree: 0,
    };
  }
}

export function symlinkSync(
  target: FilePath,
  path: FilePath,
  type: SymlinkType = null
): void {
  // We don't implement type in any meaningful way but we do validate it.
  validateOneOf(type, 'type', ['dir', 'file', 'junction', null]);
  cffs.link(normalizePath(path), normalizePath(target), { symbolic: true });
}

export function truncateSync(path: FilePath, len: number = 0): void {
  validateUint32(len, 'len');
  cffs.truncate(normalizePath(path), len);
}

export function unlinkSync(path: FilePath): void {
  cffs.unlink(normalizePath(path));
}

export function utimesSync(
  path: FilePath,
  atime: RawTime | Date,
  mtime: RawTime | Date
): void {
  // We do not actually make use of access time in our filesystem. We just
  // validate the inputs here.
  atime = getDate(atime);
  mtime = getDate(mtime);
  cffs.setLastModified(normalizePath(path), mtime, { followSymlinks: true });
}

export function writeFileSync(
  path: number | FilePath,
  data: string | ArrayBufferView,
  options: BufferEncoding | null | WriteFileOptions = {}
): number {
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
  validateEncoding(encoding, 'options.encoding');
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

  return cffs.writeAll(path, data, { append, exclusive });
}

export type WriteSyncOptions = {
  offset?: number | undefined;
  length?: number | undefined;
  position?: Position | undefined;
};

export function writeSync(
  fd: number,
  buffer: NodeJS.ArrayBufferView | string,
  offsetOrOptions: WriteSyncOptions | Position = null,
  length?: number | BufferEncoding | null,
  position?: Position
): number {
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

    length ??= buffer.byteLength - offset;

    validateInteger(length, 'length', 0);

    // Validate that the offset + length do not exceed the buffer's byte length.
    if (offset + length > buffer.byteLength) {
      throw new ERR_INVALID_ARG_VALUE('offset', offset, 'out of bounds');
    }

    return writevSync(
      fd,
      [Buffer.from(buffer.buffer, offset, length)],
      position
    );
  }

  validateStringAfterArrayBufferView(buffer, 'buffer');

  // In this case, offsetOrOptions must either be a number, bigint, or null.
  validatePosition(offsetOrOptions, 'position');
  position = offsetOrOptions;

  // In this instance, buffer is a string and the length arg specifies
  // the encoding to use.
  validateEncoding(buffer, length as string);
  buffer = Buffer.from(buffer, length as string /* encoding */);

  return writevSync(fd, [buffer], position);
}

export function writevSync(
  fd: number,
  buffers: NodeJS.ArrayBufferView[],
  position: Position = null
): number {
  fd = getValidatedFd(fd);
  validateBufferArray(buffers);
  validatePosition(position, 'position');

  if (buffers.length === 0) {
    return 0;
  }

  return cffs.write(fd, buffers, { position });
}

// An API is considered stubbed if it is not implemented by the function
// exists with the correct signature and throws an error if called. If
// a function exists that does not have the correct signature, it is
// not considered fully stubbed.
// An API is considered optimized if the API has been implemented and
// tested and then optimized for performance.
//
// (S == Stubbed, I == Implemented, T == Tested, O == Optimized, V = Verified)
// For T, 1 == basic tests, 2 == node.js tests ported
// Verified means that the behavior or the API has been verified to be
// consistent with the node.js API. This does not mean that the behaviors.
// We can only determine verification status once the node.js tests are
// ported and verified to work correctly.
// match exactly, just that they are consistent.
//  S  I  T  V  O
// [x][x][2][x][x] fs.accessSync(path[, mode])
// [x][x][2][x][x] fs.existsSync(path)
// [x][x][2][x][x] fs.chmodSync(path, mode)
// [x][x][2][x][x] fs.chownSync(path, uid, gid)
// [x][x][2][x][x] fs.closeSync(fd)
// [x][x][2][x][x] fs.fchmodSync(fd, mode)
// [x][x][2][x][x] fs.fchownSync(fd, uid, gid)
// [x][x][2][x][x] fs.lchmodSync(path, mode)
// [x][x][2][x][x] fs.lchownSync(path, uid, gid)
// [x][x][2][x][x] fs.futimesSync(fd, atime, mtime)
// [x][x][2][x][x] fs.lutimesSync(path, atime, mtime)
// [x][x][2][x][x] fs.utimesSync(path, atime, mtime)
// [x][x][2][x][x] fs.fstatSync(fd[, options])
// [x][x][2][x][x] fs.lstatSync(path[, options])
// [x][x][2][x][x] fs.statSync(path[, options])
// [x][x][2][x][x] fs.statfsSync(path[, options])
// [x][x][2][x][x] fs.fdatasyncSync(fd)
// [x][x][2][x][x] fs.fsyncSync(fd)
// [x][x][2][x][x] fs.linkSync(existingPath, newPath)
// [x][x][2][x][x] fs.readlinkSync(path[, options])
// [x][x][2][x][x] fs.realpathSync(path[, options])
// [x][x][2][x][x] fs.realpathSync.native(path[, options])
// [x][x][2][x][x] fs.symlinkSync(target, path[, type])
// [x][x][2][x][x] fs.unlinkSync(path)
//
// [x][x][1][ ][ ] fs.appendFileSync(path, data[, options])
// [x][x][1][ ][ ] fs.copyFileSync(src, dest[, mode])
// [x][ ][ ][ ][ ] fs.cpSync(src, dest[, options])
// [x][x][1][ ][ ] fs.ftruncateSync(fd[, len])
// [ ][ ][ ][ ][ ] fs.globSync(pattern[, options])
// [x][x][1][ ][ ] fs.mkdirSync(path[, options])
// [x][x][1][ ][ ] fs.mkdtempSync(prefix[, options])
// [x][ ][ ][ ][ ] fs.opendirSync(path[, options])
// [x][x][1][ ][ ] fs.openSync(path[, flags[, mode]])
// [x][x][1][ ][ ] fs.readdirSync(path[, options])
// [x][x][1][ ][ ] fs.readFileSync(path[, options])
// [x][x][1][ ][ ] fs.readSync(fd, buffer, offset, length[, position])
// [x][x][1][ ][ ] fs.readSync(fd, buffer[, options])
// [x][x][1][ ][ ] fs.readvSync(fd, buffers[, position])
// [x][x][1][ ][ ] fs.renameSync(oldPath, newPath)
// [x][x][1][ ][ ] fs.rmdirSync(path[, options])
// [x][x][1][ ][ ] fs.rmSync(path[, options])
// [x][x][1][ ][ ] fs.truncateSync(path[, len])
// [x][x][1][ ][ ] fs.writeFileSync(file, data[, options])
// [x][x][1][ ][ ] fs.writeSync(fd, buffer, offset[, length[, position]])
// [x][x][1][ ][ ] fs.writeSync(fd, buffer[, options])
// [x][x][1][ ][ ] fs.writeSync(fd, string[, position[, encoding]])
// [x][x][1][ ][ ] fs.writevSync(fd, buffers[, position])
