/* eslint-disable @typescript-eslint/no-unused-vars,@typescript-eslint/no-unnecessary-condition */

import {
  getOptions,
  kMaxUserId,
  stringToFlags,
  getValidatedFd,
  validateBufferArray,
  validateStringAfterArrayBufferView,
  normalizePath,
  Stats,
  kBadge,
  type FilePath,
  validatePosition,
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
import { type Dir, type Dirent } from 'node-internal:internal_fs';
import { default as cffs } from 'cloudflare-internal:filesystem';

import { Buffer } from 'node-internal:internal_buffer';

function accessSyncImpl(
  path: FilePath,
  mode: number = F_OK,
  followSymlinks: boolean
): void {
  validateUint32(mode, 'mode');

  // If the X_OK flag is set we will always throw because we don't
  // support executable files.
  if (mode & X_OK) {
    throw new Error('access denied'); // not executable.
  }

  const stat = cffs.stat(normalizePath(path), { followSymlinks });

  // Similar to node.js, we make no differentiation between the file
  // not existing and the file existing but not being accessible.
  if (stat == null) {
    throw new Error('access denied'); // not found.
  }

  if (mode & W_OK && !stat.writable) {
    throw new Error('access denied'); // not writable.
  }

  // We always assume that files are readable, so if we get here the
  // path is accessible.
}

export function accessSync(path: FilePath, mode: number = F_OK): void {
  accessSyncImpl(path, mode, true);
}

export type WriteFileOptions = {
  encoding?: string;
  // We don't meaningfully implement the mode option but we do validate it.
  mode?: string | number;
  // We support the `w` and `a` flags. The default is `w`.
  flag?: string;
  // We don't meaningfully implement the flush option but we do validate it.
  flush?: boolean;
};

export function appendFileSync(
  path: number | FilePath,
  data: string | ArrayBufferView,
  options: WriteFileOptions = {
    encoding: 'utf8',
    mode: 0o666,
    flag: 'a',
    flush: false,
  }
): void {
  // We defer to writeFileSync for the actual implementation and validation
  const {
    encoding = 'utf8',
    mode = 0o666,
    flag = 'a',
    flush = false,
  } = options ?? {};
  writeFileSync(path, data, { encoding, mode, flag, flush });
}

export function chmodSync(path: FilePath, mode: string | number): void {
  parseFileMode(mode, 'mode');
  accessSync(path, F_OK);
  // We do not implement chmod in any meaningful way as our filesystem
  // has no concept of user-defined permissions. Once we validate the inputs
  // we just return as a non-op.
  // The reason we call accessSync is to ensure, at the very least, that
  // the path exists and would otherwise be accessible.
}

export function chownSync(path: FilePath, uid: number, gid: number): void {
  validateInteger(uid, 'uid', -1, kMaxUserId);
  validateInteger(gid, 'gid', -1, kMaxUserId);
  accessSync(path, F_OK);
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
  src = normalizePath(src);
  dest = normalizePath(dest);
  throw new Error('Not implemented');
}

export type CopySyncOptions = {
  // Dereference symlinks when copying.
  dereference?: boolean;
  // When force is false, and the destination exists, throw an error.
  errorOnExist?: boolean;
  // Function to filter copied files/directories. Return true to copy the item,
  // false to ignore it. When ignoring a directory, all of its contents will be
  // skipped as well.
  filter?: (src: FilePath, dest: FilePath) => boolean;
  force?: boolean;
  // COPYFILE_EXCL, COPYFILE_FICLONE, COPYFILE_FICLONE_FORCE
  mode?: number;
  // When true timestamps from src will be preserved.
  preserveTimestamps?: boolean;
  // Copy directories recursively
  recursive?: boolean;
  // When true, path resolution for symlinks will be skipped
  verbatimSymlinks?: boolean;
};

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
    accessSync(path, F_OK);
    return true;
  } catch (err) {
    return false;
  }
}

export function fchmodSync(fd: number, mode: string | number): void {
  fd = getValidatedFd(fd);
  parseFileMode(mode, 'mode');
  if (cffs.stat(fd, { followSymlinks: true }) == null) {
    throw new Error('bad file descriptor'); // not found or not accessible.
  }
  // We do not implement chmod in any meaningful way as our filesystem
  // has no concept of user-defined permissions. Once we validate the inputs
  // we just return as a non-op.
  // The reason we call cffs.stat is to ensure, at the very least, that
  // the fd is valid and would otherwise be accessible.
}

export function fchownSync(fd: number, uid: number, gid: number): void {
  fd = getValidatedFd(fd);
  validateInteger(uid, 'uid', -1, kMaxUserId);
  validateInteger(gid, 'gid', -1, kMaxUserId);
  if (cffs.stat(fd, { followSymlinks: true }) == null) {
    throw new Error('bad file descriptor'); // not found or not accessible.
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
    throw new Error('bad file descriptor'); // not found or not accessible.
  }
}

export function fstatSync(
  fd: number,
  options: { bigint: boolean } = { bigint: false }
): Stats {
  fd = getValidatedFd(fd);
  validateObject(options, 'options');
  const { bigint } = options;
  validateBoolean(bigint, 'options.bigint');
  const stat = cffs.stat(fd, { followSymlinks: true });
  if (stat == null) {
    throw new Error('bad file descriptor'); // not found or not accessible.
  }
  return new Stats(kBadge, stat, { bigint });
}

export function fsyncSync(fd: number): void {
  fd = getValidatedFd(fd);
  // We do not implement fdatasync in any meaningful way. At most we
  // will validate that the fd is valid and would otherwise be accessible.
  if (cffs.stat(fd, { followSymlinks: true }) == null) {
    throw new Error('bad file descriptor'); // not found or not accessible.
  }
}

export function ftruncateSync(fd: number, len: number = 0): void {
  validateUint32(len, 'len');
  cffs.truncate(getValidatedFd(fd), len);
}

function getDate(time: string | number | bigint | Date): Date {
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

export function futimesSync(
  fd: number,
  atime: string | number | bigint | Date,
  mtime: string | number | bigint | Date
): void {
  // We do not actually make use of access time in our filesystem. We just
  // validate the inputs here.
  atime = getDate(atime);
  mtime = getDate(mtime);
  cffs.setLastModified(getValidatedFd(fd), mtime, {});
}

export function lchmodSync(path: FilePath, mode: string | number): void {
  path = normalizePath(path);
  parseFileMode(mode, 'mode');
  if (cffs.stat(normalizePath(path), { followSymlinks: false }) == null) {
    throw new Error('bad file descriptor'); // not found or not accessible.
  }
  // We do not implement chmod in any meaningful way as our filesystem
  // has no concept of user-defined permissions. Once we validate the inputs
  // we just return as a non-op.
  // The reason we call cffs.stat is to ensure, at the very least, that
  // the fd is valid and would otherwise be accessible.
}

export function lchownSync(path: FilePath, uid: number, gid: number): void {
  path = normalizePath(path);
  validateInteger(uid, 'uid', -1, kMaxUserId);
  validateInteger(gid, 'gid', -1, kMaxUserId);
  if (cffs.stat(normalizePath(path), { followSymlinks: false }) == null) {
    throw new Error('bad file descriptor'); // not found or not accessible.
  }
  // We do not implement chown in any meaningful way as our filesystem
  // has no concept of user-defined permissions. Once we validate the inputs
  // we just return as a non-op.
  // The reason we call cffs.stat is to ensure, at the very least, that
  // the fd is valid and would otherwise be accessible.
}

export function lutimesSync(
  path: FilePath,
  atime: string | number | bigint | Date,
  mtime: string | number | bigint | Date
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

export type StatOptions = {
  bigint?: boolean;
  throwIfNoEntry?: boolean;
};

export function lstatSync(
  path: FilePath,
  options: StatOptions = { bigint: false, throwIfNoEntry: true }
): Stats | undefined {
  validateObject(options, 'options');
  const { bigint = false, throwIfNoEntry = true } = options;
  validateBoolean(bigint, 'options.bigint');
  validateBoolean(throwIfNoEntry, 'options.throwIfNoEntry');
  const stat = cffs.stat(normalizePath(path), { followSymlinks: false });
  if (stat == null) {
    if (throwIfNoEntry) {
      throw new Error('file not found'); // not found or not accessible.
    }
    return undefined;
  }
  return new Stats(kBadge, stat, { bigint });
}

export type MkdirSyncOptions = {
  recursive?: boolean;
  mode?: string | number;
};

export function mkdirSync(
  path: FilePath,
  options: number | MkdirSyncOptions = {}
): string | undefined {
  const { recursive = false, mode = 0o777 } = ((): MkdirSyncOptions => {
    if (typeof options === 'number') {
      return { mode: options };
    } else {
      validateObject(options, 'options');
      return options;
    }
  })();

  validateBoolean(recursive, 'options.recursive');
  parseFileMode(mode, 'mode');

  path = normalizePath(path);

  throw new Error('Not implemented');
}

export type MkdirTempSyncOptions = {
  encoding?: string;
};

export function mkdtempSync(
  prefix: FilePath,
  options: MkdirTempSyncOptions = {}
): string {
  validateObject(options, 'options');
  const { encoding = 'utf8' } = options;
  validateEncoding(encoding, 'options.encoding');
  prefix = normalizePath(prefix);
  throw new Error('Not implemented');
}

export type OpenDirOptions = {
  encoding?: string;
  bufferSize?: number;
  recursive?: boolean;
};

export function opendirSync(
  path: FilePath,
  options: OpenDirOptions = {
    encoding: 'utf8',
    bufferSize: 32,
    recursive: false,
  }
): Dir {
  validateObject(options, 'options');
  const { encoding = 'utf8', bufferSize = 32, recursive = false } = options;
  validateEncoding(encoding, 'options.encoding');
  validateUint32(bufferSize, 'options.bufferSize');
  validateBoolean(recursive, 'options.recursive');
  path = normalizePath(path);
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

export type ReadDirOptions = {
  encoding?: string;
  withFileTypes?: boolean;
  recursive?: boolean;
};

export type ReadDirResult = string[] | Buffer[] | Dirent[];

export function readdirSync(
  path: FilePath,
  options: ReadDirOptions = {
    encoding: 'utf8',
    withFileTypes: false,
    recursive: false,
  }
): ReadDirResult {
  options = getOptions(options);
  path = normalizePath(path);
  if (options.recursive != null) {
    validateBoolean(options.recursive, 'options.recursive');
  }

  throw new Error('Not implemented');
}

export type ReadFileSyncOptions = {
  encoding?: string;
  flag?: string | number;
};

export function readFileSync(
  path: FilePath,
  options: ReadFileSyncOptions = {}
): string | Buffer {
  validateObject(options, 'options');
  const { encoding = 'utf8', flag = 'r' } = options;
  validateEncoding(encoding, 'options.encoding');
  stringToFlags(flag);

  path = normalizePath(path);
  options = getOptions(options, { flag: 'r' });
  throw new Error('Not implemented');
}

export type ReadLinkSyncOptions = {
  encoding?: string;
};

export function readlinkSync(
  path: FilePath,
  options: ReadLinkSyncOptions = {}
): string | Buffer {
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

export type ReadSyncOptions = {
  offset?: number;
  length?: number;
  position?: number | bigint | null;

  // Used only if the callback read variation.
  buffer?: ArrayBufferView;
};

// readSync is overloaded to support two different signatures:
//   fs.readSync(fd, buffer, offset, length, position)
//   fs.readSync(fd, buffer, options)
//
// fd is always a number, buffer is an ArrayBufferView, offset and length
// are numbers, and position is either a number, bigint, or null.
export function readSync(
  fd: number,
  buffer: ArrayBufferView,
  offset: number,
  length: number,
  position?: number | bigint | null
): number;
export function readSync(
  fd: number,
  buffer: ArrayBufferView,
  options?: ReadSyncOptions
): number;
export function readSync(
  fd: number,
  buffer: ArrayBufferView,
  offsetOrOptions: ReadSyncOptions | number = {},
  length?: number,
  position: number | bigint | null = null
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

  throw new Error('Not implemented');
}

export function readvSync(
  fd: number,
  buffers: ArrayBufferView[],
  position: number | bigint | null = null
): number {
  fd = getValidatedFd(fd);

  validateBufferArray(buffers);
  validatePosition(position, 'position');

  if (buffers.length === 0) {
    return 0;
  }

  throw new Error('Not implemented');
}

// TODO: Implement fs.realpathSync.native
export function realpathSync(
  p: FilePath,
  options: ReadLinkSyncOptions = {}
): string | Buffer {
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

export function renameSync(oldPath: FilePath, newPath: FilePath): void {
  oldPath = normalizePath(oldPath);
  newPath = normalizePath(newPath);
  throw new Error('Not implemented');
}

export type RmdirSyncOptions = {
  maxRetries?: number;
  recursive?: boolean;
  retryDelay?: number;
};

export function rmdirSync(
  path: FilePath,
  options: RmdirSyncOptions = {}
): void {
  validateObject(options, 'options');
  const { maxRetries = 0, recursive = false, retryDelay = 0 } = options;
  // We do not implement the maxRetries or retryDelay options in any meaningful
  // way. We just validate them.
  validateUint32(maxRetries, 'options.maxRetries');
  validateBoolean(recursive, 'options.recursive');
  validateUint32(retryDelay, 'options.retryDelay');

  path = normalizePath(path);
  validateObject(options, 'options');
  throw new Error('Not implemented');
}

export type RmSyncOptions = {
  force?: boolean;
  maxRetries?: number;
  recursive?: boolean;
  retryDelay?: number;
};

export function rmSync(path: FilePath, options: RmSyncOptions = {}): void {
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

  path = normalizePath(path);
  validateObject(options, 'options');
  throw new Error('Not implemented');
}

export function statSync(
  path: FilePath,
  options: StatOptions = { bigint: false, throwIfNoEntry: true }
): Stats | undefined {
  validateObject(options, 'options');
  const { bigint = false, throwIfNoEntry = true } = options;
  validateBoolean(bigint, 'options.bigint');
  validateBoolean(throwIfNoEntry, 'options.throwIfNoEntry');
  const stat = cffs.stat(normalizePath(path), { followSymlinks: true });
  if (stat == null) {
    if (throwIfNoEntry) {
      throw new Error('file not found'); // not found or not accessible.
    }
    return undefined;
  }
  return new Stats(kBadge, stat, { bigint });
}

export type StatFs = {
  type: number | bigint;
  bsize: number | bigint;
  blocks: number | bigint;
  bfree: number | bigint;
  bavail: number | bigint;
  files: number | bigint;
  ffree: number | bigint;
};

export function statfsSync(
  path: FilePath,
  options: { bigint?: boolean } = { bigint: false }
): StatFs {
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
  type: string | null = null
): void {
  // We don't implement type in any meaningful way but we do validate it.
  validateOneOf(type, 'type', ['dir', 'file', 'junction', null, undefined]);
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
  atime: number | string | bigint | Date,
  mtime: number | string | bigint | Date
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
  options: WriteFileOptions = {
    encoding: 'utf8',
    mode: 0o666,
    flag: 'w',
    flush: false,
  }
): void {
  if (typeof path === 'number') {
    path = getValidatedFd(path);
  } else {
    path = normalizePath(path);
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
  const newFlag = stringToFlags(flag);

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
  validateBoolean(exclusive, '');

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

  throw new Error('Not implemented');
}

export type WriteSyncOptions = {
  offset?: number;
  length?: number;
  position?: number | null;
};

export function writeSync(
  fd: number,
  buffer: ArrayBufferView | string,
  offsetOrOptions: number | WriteSyncOptions | null | bigint = null,
  length?: number | string | null,
  position?: number | bigint | null
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
  if (
    offsetOrOptions != null &&
    typeof offsetOrOptions !== 'number' &&
    typeof offsetOrOptions !== 'bigint'
  ) {
    throw new ERR_INVALID_ARG_TYPE(
      'offset',
      ['null', 'number', 'bigint'],
      offsetOrOptions
    );
  }
  position = offsetOrOptions;

  validateEncoding(buffer, length as string);
  buffer = Buffer.from(buffer, length as string /* encoding */);

  return writevSync(fd, [buffer], position);
}

export function writevSync(
  fd: number,
  buffers: ArrayBufferView[],
  position: number | null | bigint = null
): number {
  fd = getValidatedFd(fd);
  validateBufferArray(buffers);

  if (
    position != null &&
    typeof position !== 'number' &&
    typeof position !== 'bigint'
  ) {
    throw new ERR_INVALID_ARG_TYPE(
      'position',
      ['null', 'number', 'bigint'],
      position
    );
  }

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
// (S == Stubbed, I == Implemented, T == Tested, O == Optimized)
//  S  I  T  O
// [x][x][x][ ] fs.accessSync(path[, mode])
// [x][x][ ][ ] fs.appendFileSync(path, data[, options])
// [x][x][x][ ] fs.chmodSync(path, mode)
// [x][x][x][ ] fs.chownSync(path, uid, gid)
// [x][x][x][ ] fs.closeSync(fd)
// [x][ ][ ][ ] fs.copyFileSync(src, dest[, mode])
// [x][ ][ ][ ] fs.cpSync(src, dest[, options])
// [x][x][x][ ] fs.existsSync(path)
// [x][x][x][ ] fs.fchmodSync(fd, mode)
// [x][x][x][ ] fs.fchownSync(fd, uid, gid)
// [x][x][x][ ] fs.fdatasyncSync(fd)
// [x][x][x][ ] fs.fstatSync(fd[, options])
// [x][x][x][ ] fs.fsyncSync(fd)
// [x][x][x][ ] fs.ftruncateSync(fd[, len])
// [x][x][x][ ] fs.futimesSync(fd, atime, mtime)
// [ ][ ][ ][ ] fs.globSync(pattern[, options])
// [x][x][x][ ] fs.lchmodSync(path, mode)
// [x][x][x][ ] fs.lchownSync(path, uid, gid)
// [x][x][x][ ] fs.lutimesSync(path, atime, mtime)
// [x][x][x][ ] fs.linkSync(existingPath, newPath)
// [x][x][x][ ] fs.lstatSync(path[, options])
// [x][ ][ ][ ] fs.mkdirSync(path[, options])
// [x][ ][ ][ ] fs.mkdtempSync(prefix[, options])
// [x][ ][ ][ ] fs.opendirSync(path[, options])
// [x][x][x][ ] fs.openSync(path[, flags[, mode]])
// [x][ ][ ][ ] fs.readdirSync(path[, options])
// [x][ ][ ][ ] fs.readFileSync(path[, options])
// [x][x][x][ ] fs.readlinkSync(path[, options])
// [x][ ][ ][ ] fs.readSync(fd, buffer, offset, length[, position])
// [x][ ][ ][ ] fs.readSync(fd, buffer[, options])
// [x][ ][ ][ ] fs.readvSync(fd, buffers[, position])
// [x][x][x][ ] fs.realpathSync(path[, options])
// [x][x][x][ ] fs.realpathSync.native(path[, options])
// [x][ ][ ][ ] fs.renameSync(oldPath, newPath)
// [x][ ][ ][ ] fs.rmdirSync(path[, options])
// [x][ ][ ][ ] fs.rmSync(path[, options])
// [x][x][x][ ] fs.statSync(path[, options])
// [x][x][x][ ] fs.statfsSync(path[, options])
// [x][x][x][ ] fs.symlinkSync(target, path[, type])
// [x][x][x][ ] fs.truncateSync(path[, len])
// [x][x][x][ ] fs.unlinkSync(path)
// [x][x][x][ ] fs.utimesSync(path, atime, mtime)
// [x][ ][ ][ ] fs.writeFileSync(file, data[, options])
// [x][x][x][ ] fs.writeSync(fd, buffer, offset[, length[, position]])
// [x][x][x][ ] fs.writeSync(fd, buffer[, options])
// [x][x][x][ ] fs.writeSync(fd, string[, position[, encoding]])
// [x][x][x][ ] fs.writevSync(fd, buffers[, position])
