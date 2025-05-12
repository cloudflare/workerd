if (
  // eslint-disable-next-line @typescript-eslint/no-explicit-any,@typescript-eslint/no-unsafe-member-access
  !(globalThis as any).Cloudflare.compatibilityFlags['enable_node_file_system']
) {
  throw new Error('The experimental node:fs implementation is not enabled');
}

import {
  default as cffs,
  type FilePath,
  type Stat as InternalStat,
} from 'cloudflare-internal:filesystem';

import {
  validateBoolean,
  validateEncoding,
  validateInteger,
  validateObject,
  validateString,
  validateUint32,
} from 'node-internal:validators';

import { Buffer } from 'node-internal:internal_buffer';

import {
  F_OK,
  W_OK,
  X_OK,
  S_IFMT,
  S_IFREG,
  S_IFDIR,
  S_IFCHR,
  S_IFBLK,
  S_IFIFO,
  S_IFLNK,
  S_IFSOCK,
  O_EXCL,
  O_RDWR,
  O_APPEND,
  O_WRONLY,
  COPYFILE_EXCL,
  COPYFILE_FICLONE_FORCE,
  kMaxUserId,
} from 'node-internal:internal_fs_constants';

import {
  normalizePath,
  parseFileMode,
  parseOpenFlags,
  kBadge,
} from 'node-internal:internal_fs_util';

import { isArrayBufferView } from 'node-internal:internal_types';

import { ERR_INVALID_ARG_TYPE } from 'node-internal:internal_errors';

export interface StatOptions {
  bigint?: boolean;
  throwIfNoEntry?: boolean;
}

export interface StatFsOptions {
  bigint?: boolean;
}

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

  public constructor(badge: symbol, stat: InternalStat, options: StatOptions) {
    if (badge !== kBadge) {
      throw new TypeError('Illegal constructor');
    }

    // All files are readable
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

      this.atimeNs = stat.lastModified;
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

      this.atimeMs = Number(stat.lastModified) / 1_000_000;
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

export interface StatFs {
  type: number | bigint;
  bsize: number | bigint;
  blocks: number | bigint;
  bfree: number | bigint;
  bavail: number | bigint;
  files: number | bigint;
  ffree: number | bigint;
}

export function statfsSync(
  path: FilePath,
  options: StatFsOptions = {}
): StatFs {
  // We don't actually implement statfs in any meaningful way. Just return
  // a non-op object with the right shape but no meaningful values.
  normalizePath(path);
  validateObject(options, 'options');
  const { bigint = false } = options;
  validateBoolean(bigint, 'options.bigint');
  return bigint
    ? {
        type: 0n,
        bsize: 0n,
        blocks: 0n,
        bfree: 0n,
        bavail: 0n,
        files: 0n,
        ffree: 0n,
      }
    : {
        type: 0,
        bsize: 0,
        blocks: 0,
        bfree: 0,
        bavail: 0,
        files: 0,
        ffree: 0,
      };
}

export function validateDate(value: unknown, name: string): Date {
  if (typeof value === 'string' || typeof value === 'number') {
    value = new Date(value);
    if (value == 'Invalid Date') {
      throw new ERR_INVALID_ARG_TYPE(name, ['Date', 'string', 'number'], value);
    }
    return value as Date;
  }
  if (!(value instanceof Date)) {
    throw new ERR_INVALID_ARG_TYPE(name, ['Date', 'string', 'number'], value);
  }
  return value;
}

export function accessSyncImpl(
  path: FilePath,
  mode: number = F_OK,
  followSymlinks: boolean
): void {
  // If the X_OK flag is set we will always throw because we don't
  // support executable files.
  validateUint32(mode, 'mode');

  if (mode & X_OK) {
    throw new Error('access denied'); // not executable.
  }
  const stat = cffs.stat(normalizePath(path), { followSymlinks });
  if (stat == null) {
    throw new Error('access denied'); // not found.
  }

  if (mode & W_OK && !stat.writable) {
    throw new Error('access denied'); // not writable.
  }

  // We always assume that files are readable, so if we get here the
  // path is accessible.
}

export function statSyncImpl(
  path: number | FilePath,
  options: StatOptions,
  followSymlinks: boolean
): Stats | undefined {
  validateObject(options, 'options');
  const { bigint = false, throwIfNoEntry = true } = options;
  validateBoolean(bigint, 'options.bigint');
  validateBoolean(throwIfNoEntry, 'options.throwIfNoEntry');
  const stat = cffs.stat(
    typeof path === 'number' ? path : normalizePath(path),
    { followSymlinks }
  );
  if (stat) {
    return new Stats(kBadge, stat, options);
  }

  if (throwIfNoEntry) {
    throw new Error('stat failed'); // not found.
  }
  return undefined;
}

export function utimesSyncImpl(
  pathOrFd: number | FilePath,
  mtime: Date,
  followSymlinks: boolean
): void {
  validateDate(mtime, 'mtime');
  if (typeof pathOrFd === 'number') {
    cffs.setLastModified(pathOrFd, mtime, { followSymlinks });
  } else {
    cffs.setLastModified(normalizePath(pathOrFd), mtime, { followSymlinks });
  }
}

export function accessSync(path: FilePath, mode: number = F_OK): void {
  accessSyncImpl(path, mode, true);
}

export function existsSync(path: FilePath): boolean {
  try {
    accessSync(path, F_OK);
    return true;
  } catch {
    return false;
  }
}

export function chmodSync(path: FilePath, mode: string | number): void {
  // We don't actually support changing the permissions/mode of nodes in
  // our filesystem. We'll validate the input parameters and the accessibility
  // of the node, but we won't actually change anything.
  accessSync(path, W_OK);
  parseFileMode(mode, 'mode', 0o666);
}

export function chownSync(path: FilePath, uid: number, gid: number): void {
  // We don't actually support changing the ownership of nodes in our
  // filesystem. We'll validate the input parameters and the accessibility
  // of the node, but we won't actually change anything.
  accessSync(path, W_OK);
  validateInteger(uid, 'uid', -1, kMaxUserId);
  validateInteger(gid, 'gid', -1, kMaxUserId);
}

export function fchmodSync(fd: number, mode: string | number): void {
  // We don't actually support changing the permissions/mode of nodes in
  // our filesystem. We'll validate the input parameters and the accessibility
  // of the node, but we won't actually change anything.
  validateInteger(fd, 'fd', 0);
  parseFileMode(mode, 'mode', 0o666);
  // We use the non-op fsync here because it does nothing more than validate
  // that the fd is valid.
  cffs.fsync(fd);
}

export function fchownSync(fd: number, uid: number, gid: number): void {
  // We don't actually support changing the ownership of nodes in our
  // filesystem. We'll validate the input parameters and the accessibility
  // of the node, but we won't actually change anything.
  validateInteger(fd, 'fd', 0);
  validateInteger(uid, 'uid', -1, kMaxUserId);
  validateInteger(gid, 'gid', -1, kMaxUserId);
  // We use the non-op fsync here because it does nothing more than validate
  // that the fd is valid.
  cffs.fsync(fd);
}

export function statSync(
  path: FilePath,
  options: StatOptions = {}
): Stats | undefined {
  return statSyncImpl(path, options, true);
}

export function fstatSync(
  fd: number,
  options: StatOptions = {}
): Stats | undefined {
  validateInteger(fd, 'fd', 0);
  return statSyncImpl(fd, options, false);
}

export function lstatSync(
  path: FilePath,
  options: StatOptions = {}
): Stats | undefined {
  return statSyncImpl(path, options, false);
}

export function lchmodSync(path: FilePath, mode: string | number): void {
  // We don't actually support changing the permissions/mode of nodes in
  // our filesystem. We'll validate the input parameters and the accessibility
  // of the node, but we won't actually change anything.
  accessSyncImpl(path, W_OK, false);
  parseFileMode(mode, 'mode', 0o666);
}

export function lchownSync(path: FilePath, uid: number, gid: number): void {
  // We don't actually support changing the ownership of nodes in our
  // filesystem. We'll validate the input parameters and the accessibility
  // of the node, but we won't actually change anything.
  accessSyncImpl(path, W_OK, false);
  validateInteger(uid, 'uid', -1, kMaxUserId);
  validateInteger(gid, 'gid', -1, kMaxUserId);
}

export function utimesSync(
  path: FilePath,
  atime: number | string | Date,
  mtime: number | string | Date
): void {
  // We validate atime but we don't actually make use of it.
  validateDate(atime, 'atime');
  mtime = validateDate(mtime, 'mtime');
  utimesSyncImpl(path, mtime, true);
}

export function futimesSync(
  fd: number,
  atime: number | string | Date,
  mtime: number | string | Date
): void {
  // We validate atime but we don't actually make use of it.
  validateInteger(fd, 'fd', 0);
  validateDate(atime, 'atime');
  mtime = validateDate(mtime, 'mtime');
  utimesSyncImpl(fd, mtime, true);
}

export function lutimesSync(
  path: FilePath,
  atime: number | string | Date,
  mtime: number | string | Date
): void {
  // We validate atime but we don't actually make use of it.
  validateDate(atime, 'atime');
  mtime = validateDate(mtime, 'mtime');
  utimesSyncImpl(path, mtime, false);
}

export function truncateSync(path: FilePath, len: number = 0): void {
  validateInteger(len, 'len', 0);
  cffs.resize(normalizePath(path), len);
}

export function ftruncateSync(fd: number, len: number = 0): void {
  validateInteger(fd, 'fd', 0);
  validateInteger(len, 'len', 0);
  cffs.resize(fd, len);
}

export function linkSync(existingPath: FilePath, newPath: FilePath): void {
  cffs.link(normalizePath(existingPath), normalizePath(newPath));
}

export function symlinkSync(
  target: FilePath,
  path: FilePath,
  type: string | null = null
): void {
  // We ignore the type parameter as it is windows only.
  // We will at least validate type as a string if it is provided.
  if (type != null) {
    validateString(type, 'type');
  }
  cffs.symlink(normalizePath(target), normalizePath(path));
}

export function unlinkSync(path: FilePath): void {
  cffs.unlink(normalizePath(path));
}

export function realpathSync(path: FilePath): string {
  return cffs.realpath(normalizePath(path));
}

realpathSync.native = realpathSync;

export function readlinkSync(path: FilePath): string {
  return cffs.readlink(normalizePath(path));
}

export function openSync(
  path: FilePath,
  flags: string | number,
  mode: string | number = 0o666
): number {
  const url = normalizePath(path);
  const openFlags = parseOpenFlags(flags, 'flags');
  // We don't actually support modes but we will validate the inputs.
  parseFileMode(mode, 'mode', 0o666);

  return cffs.open(url, {
    exclusive: Boolean(openFlags & O_EXCL),
    read: !(openFlags & O_WRONLY),
    write: Boolean(openFlags & O_WRONLY) || Boolean(openFlags & O_RDWR),
    append: Boolean(openFlags & O_APPEND),
  });
}

export function closeSync(fd: number): void {
  validateInteger(fd, 'fd', 0);
  cffs.close(fd);
}

export function fsyncSync(fd: number): void {
  validateInteger(fd, 'fd', 0);
  cffs.fsync(fd);
}

export function fdatasyncSync(fd: number): void {
  validateInteger(fd, 'fd', 0);
  // We use the non-op fsync for both fsync and fdatasync.
  cffs.fsync(fd);
}

export interface ReadFileOptions {
  encoding: string | undefined;
  flag: string | number | null;
}

export function readFileSync(
  pathOrFd: number | FilePath,
  options: ReadFileOptions = {
    encoding: undefined,
    flag: null,
  }
): Buffer | string {
  validateObject(options, 'options');
  const { encoding, flag = null } = options;
  if (encoding !== undefined) {
    validateString(encoding, 'options.encoding');
  }
  // We don't support use of flags here currently but we validate the input.
  parseOpenFlags(flag, 'options.flag');

  const u8 = ((): Uint8Array => {
    if (typeof pathOrFd === 'number') {
      return cffs.readfile(pathOrFd);
    }
    return cffs.readfile(normalizePath(pathOrFd));
  })();

  const buffer = Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength);
  if (encoding === undefined) return buffer;
  return buffer.toString(encoding);
}

export function readvSync(
  fd: number,
  buffers: ArrayBufferView[],
  position?: number
): number {
  validateInteger(fd, 'fd', 0);
  if (position !== undefined) {
    validateInteger(position, 'position', 0);
  }
  return cffs.readv(fd, buffers, { position });
}

export interface ReadSyncOptions {
  offset: number | undefined;
  length: number | undefined;
  position: number | undefined;
}

export function readSyncImpl(
  fd: number,
  buffer: ArrayBufferView,
  options: ReadSyncOptions = {
    offset: 0,
    length: buffer.byteLength,
    position: undefined,
  }
): number {
  validateInteger(fd, 'fd', 0);
  validateObject(options, 'options');
  const { offset, position } = options;
  let { length } = options;

  validateUint32(offset, 'offset');
  length ??= buffer.byteLength - offset;
  validateUint32(length, 'length');
  if (offset + length > buffer.byteLength) {
    throw new RangeError('offset + length > buffer.byteLength');
  }
  if (position !== undefined) {
    validateUint32(position, 'position');
  }
  const u8 = new Uint8Array(buffer.buffer, offset, length);
  return cffs.readv(fd, [u8], { position });
}

export function readSync(
  fd: number,
  buffer: ArrayBufferView,
  offset: number | ReadSyncOptions = 0,
  length?: number,
  position?: number
): number {
  validateInteger(fd, 'fd', 0);
  if (typeof offset === 'number') {
    return readSyncImpl(fd, buffer, { offset, length, position });
  }
  validateObject(offset, 'offset');
  return readSyncImpl(fd, buffer, offset);
}

export interface WriteFileOptions {
  encoding: string;
  mode: string | number | undefined;
  flag: string | number | undefined;
  flush?: boolean;
}

export function appendFileSync(
  fileOrFd: number | FilePath,
  data: string | ArrayBufferView,
  options: WriteFileOptions = {
    encoding: 'utf8',
    mode: 0o666,
    flag: 'a',
    flush: false,
  }
): void {
  options.flag ??= 'a';
  writeFileSync(fileOrFd, data, options);
}

export function writeFileSync(
  fileOrFd: number | FilePath,
  data: string | ArrayBufferView,
  options: WriteFileOptions = {
    encoding: 'utf8',
    mode: 0o666,
    flag: 'w',
    flush: false,
  }
): void {
  validateObject(options, 'options');
  const {
    encoding = 'utf8',
    mode = 0o666,
    flag = 'w',
    flush = false,
  } = options;
  validateEncoding(encoding, 'options.encoding');

  // We don't support the mode or flush options here
  // but we do validate the inputs.
  parseFileMode(mode, 'options.mode', 0o666);
  validateBoolean(flush, 'options.flush');
  const openFlag = parseOpenFlags(flag, 'options.flag');
  const append = Boolean(openFlag & O_APPEND);

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

  if (typeof fileOrFd === 'number') {
    cffs.writeFile(fileOrFd, data, { append });
  } else {
    cffs.writeFile(normalizePath(fileOrFd), data, { append });
  }
}

export function writevSync(
  fd: number,
  buffers: ArrayBufferView[],
  position?: number
): number {
  validateInteger(fd, 'fd', 0);
  if (position !== undefined) {
    validateInteger(position, 'position', 0);
  }
  return cffs.writev(fd, buffers, { position });
}

export interface WriteSyncOptions {
  offset?: number;
  length?: number;
  position?: number;
}

export function writeSync(
  fd: number,
  buffer: string | ArrayBufferView,
  offsetPositionOrOptions: number | WriteSyncOptions = 0,
  lengthOrEncoding: number | string | undefined,
  position: number | undefined
): number {
  // Node does not make this one easy. There are three different signatures
  // for this function.

  validateInteger(fd, 'fd', 0);

  // This is the first one: fd, string[, position[, encoding]]
  if (typeof buffer === 'string') {
    // offsetPositionOrOptions must be position if it is not undefined
    // lengtOrEncoding must be encoding if it is not undefined
    validateUint32(offsetPositionOrOptions, 'position');
    if (lengthOrEncoding !== undefined) {
      validateEncoding(lengthOrEncoding, 'encoding');
    }
    return cffs.writev(fd, [Buffer.from(buffer, lengthOrEncoding as string)], {
      position: offsetPositionOrOptions,
    });
  }

  if (!isArrayBufferView(buffer)) {
    throw new ERR_INVALID_ARG_TYPE(
      'buffer',
      ['string', 'Buffer', 'TypedArray', 'DataView'],
      buffer
    );
  }

  let offset: number;
  let length: number;

  if (typeof offsetPositionOrOptions === 'number') {
    offset = offsetPositionOrOptions;
    length =
      lengthOrEncoding !== undefined
        ? (lengthOrEncoding as number)
        : buffer.byteLength - offset;
  } else {
    validateObject(offsetPositionOrOptions, 'options');
    const options = offsetPositionOrOptions;
    offset = options.offset !== undefined ? (options.offset as number) : 0;
    length =
      options.length !== undefined
        ? (options.length as number)
        : buffer.byteLength - offset;
    position = options.position as number | undefined;
  }

  validateUint32(offset, 'offset');
  validateUint32(length, 'length');
  if (position !== undefined) {
    validateUint32(position, 'position');
  }

  if (length == 0) return 0;

  return cffs.writev(fd, [new Uint8Array(buffer.buffer, offset, length)], {
    position,
  });
}

export function copyFileSync(
  src: FilePath,
  dest: FilePath,
  mode: number = 0
): void {
  validateUint32(mode, 'mode');
  const exclusive = Boolean(mode & COPYFILE_EXCL);

  if (mode & COPYFILE_FICLONE_FORCE) {
    throw new Error('COPYFILE_FICLONE not supported');
  }

  cffs.copyFile(normalizePath(src), normalizePath(dest), { exclusive });
}

export function renameSync(src: FilePath, dest: FilePath): void {
  cffs.rename(normalizePath(src), normalizePath(dest));
}

//  P  T  O   ( P == Present, T == Tested, O == Optimized)
// [x][x][x] fs.accessSync(path[, mode])
// [x][ ][ ] fs.chmodSync(path, mode)
// [x][ ][ ] fs.chownSync(path, uid, gid)
// [x][x][x] fs.existsSync(path)
// [x][ ][ ] fs.lchmodSync(path, mode)
// [x][ ][ ] fs.lchownSync(path, uid, gid)
// [x][ ][ ] fs.lstatSync(path[, options])
// [x][ ][ ] fs.statSync(path[, options])
// [x][ ][ ] fs.utimesSync(path, atime, mtime)
// [x][ ][ ] fs.lutimesSync(path, atime, mtime)
// [x][ ][ ] fs.truncateSync(path[, len])
// [x][ ][ ] fs.linkSync(existingPath, newPath)
// [x][ ][ ] fs.symlinkSync(target, path[, type])
// [x][ ][ ] fs.unlinkSync(path)
// [x][ ][ ] fs.readlinkSync(path[, options])
// [x][ ][ ] fs.realpathSync(path[, options])
// [x][ ][ ] fs.realpathSync.native(path[, options])
// [x][ ][ ] fs.statfsSync(path[, options])
// [x][ ][ ] fs.fstatSync(fd[, options])
// [x][ ][ ] fs.fdatasyncSync(fd)
// [x][ ][ ] fs.fsyncSync(fd)
// [x][ ][ ] fs.fchmodSync(fd, mode)
// [x][ ][ ] fs.fchownSync(fd, uid, gid)
// [x][ ][ ] fs.ftruncateSync(fd[, len])
// [x][ ][ ] fs.futimesSync(fd, atime, mtime)
// [x][ ][ ] fs.closeSync(fd)
// [x][ ][ ] fs.openSync(path[, flags[, mode]])
// [x][ ][ ] fs.readFileSync(path[, options])
// [x][ ][ ] fs.readvSync(fd, buffers[, position])
// [x][ ][ ] fs.readSync(fd, buffer, offset, length[, position])
// [x][ ][ ] fs.readSync(fd, buffer[, options])
// [x][ ][ ] fs.writeFileSync(file, data[, options])
// [x][ ][ ] fs.appendFileSync(path, data[, options])
// [x][ ][ ] fs.writevSync(fd, buffers[, position])
// [x][ ][ ] fs.writeSync(fd, buffer, offset[, length[, position]])
// [x][ ][ ] fs.writeSync(fd, buffer[, options])
// [x][ ][ ] fs.writeSync(fd, string[, position[, encoding]])
// [x][ ][ ] fs.copyFileSync(src, dest[, mode])
// [x][ ][ ] fs.renameSync(oldPath, newPath)

// [ ][ ][ ] fs.cpSync(src, dest[, options])
// [ ][ ][ ] fs.mkdirSync(path[, options])
// [ ][ ][ ] fs.mkdtempSync(prefix[, options])
// [ ][ ][ ] fs.opendirSync(path[, options])
// [ ][ ][ ] fs.readdirSync(path[, options])
// [ ][ ][ ] fs.rmdirSync(path[, options])
// [ ][ ][ ] fs.rmSync(path[, options])
// [ ][ ][ ] fs.globSync(pattern[, options])
