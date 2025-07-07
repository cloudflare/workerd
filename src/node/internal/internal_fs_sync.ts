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
  normalizePath,
  getDate,
  Stats,
  kBadge,
  type FilePath,
  type Position,
  type RawTime,
  type SymlinkType,
  type ReadDirOptions,
  type WriteSyncOptions,
  validatePosition,
  validateAccessArgs,
  validateChownArgs,
  validateChmodArgs,
  validateStatArgs,
  validateMkDirArgs,
  validateRmArgs,
  validateRmDirArgs,
  validateReaddirArgs,
  validateReadArgs,
  validateWriteArgs,
  validateWriteFileArgs,
  validateOpendirArgs,
} from 'node-internal:internal_fs_utils';
import {
  parseFileMode,
  validateBoolean,
  validateObject,
  validateOneOf,
  validateUint32,
} from 'node-internal:validators';
import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_ENOENT,
  ERR_EBADF,
  ERR_EINVAL,
  ERR_EEXIST,
  ERR_UNSUPPORTED_OPERATION,
} from 'node-internal:internal_errors';

import {
  F_OK,
  X_OK,
  W_OK,
  O_WRONLY,
  O_RDWR,
  O_APPEND,
  O_EXCL,
  COPYFILE_EXCL,
  COPYFILE_FICLONE,
  COPYFILE_FICLONE_FORCE,
} from 'node-internal:internal_fs_constants';
import { Dir, Dirent } from 'node-internal:internal_fs';
import { default as cffs } from 'cloudflare-internal:filesystem';

import { Buffer } from 'node-internal:internal_buffer';
import type {
  BigIntStatsFs,
  CopySyncOptions,
  GlobOptions,
  GlobOptionsWithFileTypes,
  GlobOptionsWithoutFileTypes,
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
  mode: number = 0
): void {
  validateOneOf(mode, 'mode', [
    0,
    COPYFILE_EXCL,
    COPYFILE_FICLONE_FORCE,
    COPYFILE_FICLONE,
  ]);
  if (mode & COPYFILE_FICLONE_FORCE) {
    throw new ERR_UNSUPPORTED_OPERATION();
  }
  if (mode & COPYFILE_EXCL && existsSync(dest)) {
    throw new ERR_EEXIST({ syscall: 'copyFile' });
  }
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

  // We do not implement the filter option currently. There's a bug in the Node.js
  // implementation of fs.cp and the option.filter in which non-UTF-8 encoded file
  // names are not handled correctly and the option.filter fails when the src or
  // dest is passed in as a Buffer. Fixing this bug in Node.js will require a breaking
  // change to the API or a new API that appropriately handles Buffer inputs and non
  // UTF-8 encoded names. We want to avoid implementing the filter option for now
  // until Node.js settles on a better implementation and API.
  if (filter !== undefined) {
    if (typeof filter !== 'function') {
      throw new ERR_INVALID_ARG_TYPE('options.filter', 'function', filter);
    }
    throw new ERR_UNSUPPORTED_OPERATION();
  }

  const exclusive = Boolean(mode & COPYFILE_EXCL);
  // We're not current implementing the exclusive flag. We're validating
  // it here just to use it so the compiler doesn't complain.
  validateBoolean(exclusive, '');

  // We're not currently implementing verbatimSymlinks in any meaningful way.
  // Our symlinks are always fully qualfied. That is, they always point to
  // an absolute path and never to a relative path, so there is no distinction
  // between verbatimSymlinks and non-verbatimSymlinks. We validate the option
  // value above but otherwise we ignore it.

  // We're also not currently implementing the preserveTimestamps option.
  // Timestamps in our virtual filesystem aren't super meaningful given
  // that most files in the current implementation are either created
  // at startup and use the EPOCH as their timestamp, or are temporary files
  // that are deleted when the request completes.
  // TODO(node-fs): Decide if we want to implement preserveTimestamps in the future.

  cffs.cp(normalizePath(src), normalizePath(dest), {
    deferenceSymlinks: dereference,
    recursive,
    force,
    errorOnExist,
  });
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
  const { path: normalizedPath, recursive } = validateMkDirArgs(path, options);

  return cffs.mkdir(normalizedPath, { recursive, tmp: false });
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
  if (!Buffer.isEncoding(encoding) && encoding !== 'buffer') {
    throw new ERR_INVALID_ARG_VALUE('options.encoding', encoding);
  }
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
  const {
    path: validatedPath,
    encoding,
    recursive,
  } = validateOpendirArgs(path, options);

  const handles = cffs.readdir(validatedPath, { recursive });
  return new Dir(handles, path, { encoding });
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

export type ReadDirResult = string[] | Buffer[] | Dirent[];

export function readdirSync(
  path: FilePath,
  options: BufferEncoding | 'buffer' | null | ReadDirOptions = {}
): ReadDirResult {
  if (typeof options === 'string' || options == null) {
    options = { encoding: options };
  }
  const {
    path: normalizedPath,
    encoding,
    withFileTypes,
    recursive,
  } = validateReaddirArgs(path, options);

  const handles = cffs.readdir(normalizedPath, { recursive });

  if (withFileTypes) {
    if ((encoding as string) === 'buffer') {
      return handles.map((handle) => {
        return new Dirent(
          Buffer.from(handle.name),
          handle.type,
          handle.parentPath
        );
      });
    }
    return handles.map((handle) => {
      return new Dirent(handle.name, handle.type, handle.parentPath);
    });
  }

  if ((encoding as string) === 'buffer') {
    return handles.map((handle) => {
      return Buffer.from(handle.name);
    });
  }

  return handles.map((handle) => {
    return Buffer.from(handle.name).toString(encoding);
  });
}

export type ReadFileSyncOptions = {
  encoding?: BufferEncoding | 'buffer' | null | undefined;
  flag?: string | number | undefined;
};

export function readFileSync(
  pathOrFd: number | FilePath,
  options: BufferEncoding | 'buffer' | null | ReadFileSyncOptions = {}
): string | Buffer {
  if (typeof options === 'string' || options == null) {
    options = { encoding: options };
  }
  validateObject(options, 'options');
  const { encoding = null, flag = 'r' } = options;
  if (
    encoding !== null &&
    encoding !== 'buffer' &&
    !Buffer.isEncoding(encoding)
  ) {
    throw new ERR_INVALID_ARG_VALUE('options.encoding', encoding);
  }
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
  encoding?: BufferEncoding | 'buffer' | null | undefined;
};

export function readlinkSync(
  path: FilePath,
  options: BufferEncoding | 'buffer' | null | ReadLinkSyncOptions = {}
): string | Buffer {
  if (typeof options === 'string' || options == null) {
    options = { encoding: options };
  }
  validateObject(options, 'options');
  const { encoding = 'utf8' } = options;
  if (!Buffer.isEncoding(encoding) && encoding !== 'buffer') {
    throw new ERR_INVALID_ARG_VALUE('options.encoding', encoding);
  }
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
  const {
    fd: validatedFd,
    buffer: actualBuffer,
    length: actualLength,
    position: actualPosition,
  } = validateReadArgs(fd, buffer, offsetOrOptions, length, position);

  if (actualLength === 0 || buffer.byteLength === 0) {
    return 0;
  }

  return readvSync(validatedFd, actualBuffer, actualPosition);
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
  if (!Buffer.isEncoding(encoding) && encoding !== 'buffer') {
    throw new ERR_INVALID_ARG_VALUE('options.encoding', encoding);
  }
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
  const { path: normalizedPath, recursive } = validateRmDirArgs(path, options);

  cffs.rm(normalizedPath, { recursive, force: false, dironly: true });
}

export function rmSync(path: FilePath, options: RmOptions = {}): void {
  const {
    path: normalizedPath,
    recursive,
    force,
  } = validateRmArgs(path, options);

  cffs.rm(normalizedPath, { recursive, force, dironly: false });
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
  const {
    path: validatedPath,
    data: actualData,
    append,
    exclusive,
  } = validateWriteFileArgs(path, data, options);

  return cffs.writeAll(validatedPath, actualData, { append, exclusive });
}

export function writeSync(
  fd: number,
  buffer: NodeJS.ArrayBufferView | string,
  offsetOrOptions: WriteSyncOptions | Position = null,
  length?: number | BufferEncoding | null,
  position?: Position
): number {
  const {
    fd: validatedFd,
    buffer: actualBuffer,
    position: actualPosition,
  } = validateWriteArgs(fd, buffer, offsetOrOptions, length, position);

  return writevSync(validatedFd, actualBuffer, actualPosition);
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

export function globSync(
  _pattern: string | readonly string[],
  _options:
    | GlobOptions
    | GlobOptionsWithFileTypes
    | GlobOptionsWithoutFileTypes = {}
): string[] {
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
// [x][x][2][x][x] fs.mkdirSync(path[, options])
// [x][x][2][x][x] fs.mkdtempSync(prefix[, options])
// [x][x][2][x][x] fs.rmdirSync(path[, options])
// [x][x][2][x][x] fs.rmSync(path[, options])
// [x][x][2][x][x] fs.ftruncateSync(fd[, len])
// [x][x][2][x][x] fs.truncateSync(path[, len])
// [x][x][2][x][x] fs.openSync(path[, flags[, mode]])
// [x][x][2][x][x] fs.readdirSync(path[, options])
// [x][x][2][x][x] fs.readFileSync(path[, options])
// [x][x][2][x][x] fs.readSync(fd, buffer, offset, length[, position])
// [x][x][2][x][x] fs.readSync(fd, buffer[, options])
// [x][x][2][x][x] fs.readvSync(fd, buffers[, position])
// [x][x][2][x][x] fs.renameSync(oldPath, newPath)
// [x][x][2][x][x] fs.writeFileSync(file, data[, options])
// [x][x][2][x][x] fs.writeSync(fd, buffer, offset[, length[, position]])
// [x][x][2][x][x] fs.writeSync(fd, buffer[, options])
// [x][x][2][x][x] fs.writeSync(fd, string[, position[, encoding]])
// [x][x][2][x][x] fs.writevSync(fd, buffers[, position])
// [x][x][2][x][x] fs.appendFileSync(path, data[, options])
// [x][x][2][x][x] fs.copyFileSync(src, dest[, mode])
// [x][x][2][x][x] fs.opendirSync(path[, options])
// [x][x][2][x][x] fs.cpSync(src, dest[, options])
// [ ][ ][ ][ ][ ] fs.globSync(pattern[, options])
