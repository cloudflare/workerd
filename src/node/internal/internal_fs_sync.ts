/* eslint-disable @typescript-eslint/no-unused-vars,@typescript-eslint/no-unnecessary-condition */

import {
  getValidatedPath,
  getOptions,
  copyObject,
  kMaxUserId,
  validateCpOptions,
  toUnixTimestamp,
  stringToFlags,
  validatePosition,
  validateOffsetLengthRead,
  getValidatedFd,
  validateBufferArray,
  validatePath,
  validateStringAfterArrayBufferView,
  validateOffsetLengthWrite,
} from 'node-internal:internal_fs_utils';
import {
  isInt32,
  validateInteger,
  parseFileMode,
  validateBoolean,
  validateObject,
  validateBuffer,
  kValidateObjectAllowNullable,
  validateOneOf,
  validateEncoding,
  validateNumber,
} from 'node-internal:validators';
import { ERR_INVALID_ARG_VALUE } from 'node-internal:internal_errors';
import { posix } from 'node-internal:internal_path';
import { toPathIfFileURL } from 'node-internal:internal_url';
import { isArrayBufferView } from 'node-internal:internal_types';

import type { CopySyncOptions, ReadSyncOptions, ReadPosition } from 'node:fs';

type FilePath = string | URL;

export function accessSync(path: FilePath, _mode: number): void {
  path = getValidatedPath(path);
  throw new Error('Not implemented');
}

export function appendFileSync(
  path: FilePath,
  data: ArrayBufferView,
  options: Record<string, unknown>
): void {
  options = getOptions(options, { encoding: 'utf8', mode: 0o666, flag: 'a' });

  // Don't make changes directly on options object
  options = copyObject(options);

  // Force append behavior when using a supplied file descriptor
  if (!options.flag || isInt32(path)) options.flag = 'a';

  writeFileSync(path, data, options);
}

export function chmodSync(path: FilePath, mode: number): void {
  path = getValidatedPath(path);
  mode = parseFileMode(mode, 'mode');

  throw new Error('Not implemented');
}

export function chownSync(path: FilePath, uid: number, gid: number): void {
  path = getValidatedPath(path);
  validateInteger(uid, 'uid', -1, kMaxUserId);
  validateInteger(gid, 'gid', -1, kMaxUserId);
  throw new Error('Not implemented');
}

export function closeSync(fd: number): void {
  validateNumber(fd, 'fd');
  throw new Error('Not implemented');
}

export function copyFileSync(
  src: FilePath,
  dest: FilePath,
  _mode: number
): void {
  src = getValidatedPath(src, 'src');
  dest = getValidatedPath(dest, 'dest');
  throw new Error('Not implemented');
}

export function cpSync(
  src: FilePath,
  dest: FilePath,
  options: Record<string, unknown> | CopySyncOptions
): void {
  options = validateCpOptions(options) as CopySyncOptions;
  src = getValidatedPath(src, 'src');
  dest = getValidatedPath(dest, 'dest');
  throw new Error('Not implemented');
}

export function existsSync(path: FilePath): boolean {
  try {
    path = getValidatedPath(path);
  } catch (err) {
    return false;
  }

  throw new Error('Not implemented');
}

export function fchmodSync(_fd: number, mode: number): void {
  mode = parseFileMode(mode, 'mode');
  throw new Error('Not implemented');
}

export function fchownSync(_fd: number, uid: number, gid: number): void {
  validateInteger(uid, 'uid', -1, kMaxUserId);
  validateInteger(gid, 'gid', -1, kMaxUserId);
  throw new Error('Not implemented');
}

export function fdatasyncSync(fd: number): void {
  validateNumber(fd, 'fd');
  throw new Error('Not implemented');
}

export function fstatSync(
  fd: number,
  _options: Record<string, unknown> = { bigint: false }
): void {
  validateNumber(fd, 'fd');
  throw new Error('Not implemented');
}

export function fsyncSync(fd: number): void {
  validateNumber(fd, 'fd');
  throw new Error('Not implemented');
}

export function ftruncateSync(fd: number, len: number = 0): void {
  validateNumber(fd, 'fd');
  validateInteger(len, 'len');
  throw new Error('Not implemented');
}

export function futimesSync(
  fd: number,
  atime: string | number | Date,
  mtime: string | number | Date
): void {
  validateNumber(fd, 'fd');
  atime = atime instanceof Date ? atime.getTime() : atime;
  mtime = mtime instanceof Date ? mtime.getTime() : mtime;
  throw new Error('Not implemented');
}

export function lchmodSync(path: FilePath, _mode: number): void {
  path = getValidatedPath(path);
  throw new Error('Not implemented');
}

export function lchownSync(path: FilePath, uid: number, gid: number): void {
  path = getValidatedPath(path);
  validateInteger(uid, 'uid', -1, kMaxUserId);
  validateInteger(gid, 'gid', -1, kMaxUserId);
  throw new Error('Not implemented');
}

export function lutimesSync(
  path: FilePath,
  atime: string | number | Date,
  mtime: string | number | Date
): void {
  path = getValidatedPath(path);
  atime = toUnixTimestamp(atime);
  mtime = toUnixTimestamp(mtime);
  throw new Error('Not implemented');
}

export function linkSync(existingPath: FilePath, newPath: FilePath): void {
  existingPath = getValidatedPath(existingPath, 'existingPath');
  newPath = getValidatedPath(newPath, 'newPath');
  throw new Error('Not implemented');
}

export function lstatSync(
  path: FilePath,
  _options: Record<string, unknown> = { bigint: false, throwIfNoEntry: true }
): void {
  path = getValidatedPath(path);
  throw new Error('Not implemented');
}

export function mkdirSync(
  path: FilePath,
  options?: Record<string, unknown> | number | string
): void {
  // @ts-expect-error TS6133 Declared value not used.
  let mode = 0o777;
  // @ts-expect-error TS6133 Declared value not used.
  let recursive = false;
  if (typeof options === 'number' || typeof options === 'string') {
    mode = parseFileMode(options, 'mode');
  } else if (options) {
    if (options.recursive !== undefined) {
      validateBoolean(options.recursive, 'options.recursive');
      recursive = options.recursive;
    }
    if (options.mode !== undefined) {
      mode = parseFileMode(options.mode, 'options.mode');
    }
  }
  path = getValidatedPath(path);

  throw new Error('Not implemented');
}

export function mkdtempSync(
  prefix: FilePath,
  options: Record<string, unknown>
): void {
  options = getOptions(options);
  prefix = getValidatedPath(prefix, 'prefix');
  throw new Error('Not implemented');
}

export function opendirSync(path: FilePath): void {
  path = getValidatedPath(path);
  throw new Error('Not implemented');
}

export function openSync(
  path: FilePath,
  flags: number | null | string,
  mode: number
): void {
  path = getValidatedPath(path);
  // @ts-expect-error TS6133 Argument declared but not used
  const newFlags = stringToFlags(flags);
  mode = parseFileMode(mode, 'mode', 0o666);
  throw new Error('Not implemented');
}

export function readdirSync(
  path: FilePath,
  options: Record<string, unknown>
): void {
  options = getOptions(options);
  path = getValidatedPath(path);
  if (options.recursive != null) {
    validateBoolean(options.recursive, 'options.recursive');
  }

  throw new Error('Not implemented');
}

export function readFileSync(
  path: FilePath,
  options: Record<string, unknown>
): void {
  path = getValidatedPath(path);
  options = getOptions(options, { flag: 'r' });
  throw new Error('Not implemented');
}

export function readlinkSync(
  path: FilePath,
  options: Record<string, unknown>
): void {
  path = getValidatedPath(path);
  options = getOptions(options);
  throw new Error('Not implemented');
}

export function readSync(
  fd: number,
  buffer: unknown,
  offsetOrOptions: ReadSyncOptions | number | undefined,
  length: number,
  position: ReadPosition | null
): number {
  fd = getValidatedFd(fd);

  validateBuffer(buffer);

  let offset: number | undefined = offsetOrOptions as number;
  if (arguments.length <= 3 || typeof offsetOrOptions === 'object') {
    if (offsetOrOptions !== undefined) {
      validateObject(offsetOrOptions, 'options', kValidateObjectAllowNullable);
    }

    ({
      offset = 0,
      length = buffer.byteLength - offset,
      position = null,
    } = (offsetOrOptions as ReadSyncOptions | null) ?? {});
  }

  if (offset === undefined) {
    offset = 0;
  } else {
    validateInteger(offset, 'offset', 0);
  }

  length |= 0;

  if (length === 0) {
    return 0;
  }

  if (buffer.byteLength === 0) {
    throw new ERR_INVALID_ARG_VALUE(
      'buffer',
      buffer,
      'is empty and cannot be written'
    );
  }

  validateOffsetLengthRead(offset, length, buffer.byteLength);

  if (position == null) {
    position = -1;
  } else {
    validatePosition(position, 'position', length);
  }

  throw new Error('Not implemented');
}

export function readvSync(
  fd: number,
  buffers: Buffer[],
  position: number | null
): void {
  fd = getValidatedFd(fd);
  validateBufferArray(buffers);

  if (typeof position !== 'number') {
    position = null;
  }

  throw new Error('Not implemented');
}

// TODO: Implement fs.realpathSync.native
export function realpathSync(
  p: FilePath,
  options: Record<string, unknown>
): void {
  options = getOptions(options);
  let path = toPathIfFileURL(p);
  if (typeof path !== 'string') {
    // eslint-disable-next-line @typescript-eslint/restrict-plus-operands
    p += '';
  }
  validatePath(path);
  path = posix.resolve(path);

  throw new Error('Not implemented');
}

export function renameSync(oldPath: FilePath, newPath: FilePath): void {
  oldPath = getValidatedPath(oldPath, 'oldPath');
  newPath = getValidatedPath(newPath, 'newPath');
  throw new Error('Not implemented');
}

export function rmdirSync(
  path: FilePath,
  _options: Record<string, unknown>
): void {
  path = getValidatedPath(path);

  throw new Error('Not implemented');
}

export function rmSync(
  path: FilePath,
  _options: Record<string, unknown>
): void {
  path = getValidatedPath(path);
  throw new Error('Not implemented');
}

export function statSync(
  path: FilePath,
  _options: Record<string, unknown> = { bigint: false, throwIfNoEntry: true }
): void {
  path = getValidatedPath(path);
  throw new Error('Not implemented');
}

export function statfsSync(
  path: FilePath,
  _options: Record<string, unknown> = { bigint: false }
): void {
  path = getValidatedPath(path);
  throw new Error('Not implemented');
}

export function symlinkSync(
  target: FilePath,
  path: FilePath,
  type?: string
): void {
  validateOneOf(type, 'type', ['dir', 'file', 'junction', null, undefined]);
  target = getValidatedPath(target, 'target');
  path = getValidatedPath(path);
  throw new Error('Not implemented');
}

export function truncateSync(path: FilePath, len?: number): void {
  path = getValidatedPath(path);
  if (len === undefined) {
    len = 0;
  }
  throw new Error('Not implemented');
}

export function unlinkSync(path: FilePath): void {
  path = getValidatedPath(path);
  throw new Error('Not implemented');
}

export function utimesSync(
  path: FilePath,
  atime: number | string | Date,
  mtime: number | string | Date
): void {
  path = getValidatedPath(path);
  atime = toUnixTimestamp(atime);
  mtime = toUnixTimestamp(mtime);
  throw new Error('Not implemented');
}

export function writeFileSync(
  path: FilePath,
  data: ArrayBufferView,
  options: Record<string, unknown>
): void {
  path = getValidatedPath(path);
  options = getOptions(options, {
    encoding: 'utf8',
    mode: 0o666,
    flag: 'w',
    flush: false,
  });

  const flush = options.flush ?? false;

  validateBoolean(flush, 'options.flush');

  if (!isArrayBufferView(data)) {
    validateStringAfterArrayBufferView(data, 'data');
    // @ts-expect-error TS2769 Overloaded type doesn't exist.
    data = Buffer.from(data, options.encoding ?? 'utf8');
  }

  throw new Error('Not implemented');
}

type WriteSyncOptions = {
  offset?: number;
  length?: number;
  position?: number | null;
};
export function writeSync(
  fd: number,
  buffer: ArrayBufferView | string,
  offsetOrOptions?: number | Record<string, unknown> | null,
  length?: number | string | null,
  position?: number | null
): void {
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
    if (position === undefined) position = null;
    if (offset == null) {
      offset = 0;
    } else {
      validateInteger(offset, 'offset', 0);
    }
    if (typeof length !== 'number') length = buffer.byteLength - offset;
    validateOffsetLengthWrite(offset, length, buffer.byteLength);
  } else {
    validateStringAfterArrayBufferView(buffer, 'buffer');
    validateEncoding(buffer, length as string);
  }
  throw new Error('Not implemented');
}

export function writevSync(
  fd: number,
  buffers: ArrayBufferView[],
  _position?: number | null
): number {
  fd = getValidatedFd(fd);
  validateBufferArray(buffers);

  if (buffers.length === 0) {
    return 0;
  }

  throw new Error('Not implemented');
}
