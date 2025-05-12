import { type FilePath } from 'cloudflare-internal:filesystem';

import { Buffer } from 'node-internal:internal_buffer';

import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
} from 'node-internal:internal_errors';

import {
  O_APPEND,
  O_CREAT,
  O_EXCL,
  O_RDONLY,
  O_RDWR,
  O_SYNC,
  O_TRUNC,
  O_WRONLY,
} from 'node-internal:internal_fs_constants';

import { validateInt32, validateUint32 } from 'node-internal:validators';

const octalReg = /^[0-7]+$/;
const modeDesc = 'must be a 32-bit unsigned integer or an octal string';

// A non-public symbol used to ensure that certain constructors cannot
// be called from user-code
export const kBadge = Symbol('kBadge');

export function normalizePath(path: FilePath): URL {
  if (typeof path === 'string') {
    return new URL(path, 'file://');
  } else if (path instanceof URL) {
    return path;
  } else if (Buffer.isBuffer(path)) {
    return new URL(path.toString(), 'file://');
  }
  throw new ERR_INVALID_ARG_TYPE('path', ['string', 'Buffer', 'URL'], path);
}

export function parseFileMode(
  value: string | number,
  name: string,
  def: number
): number {
  value = def;
  if (typeof value === 'string') {
    if (octalReg.exec(value) === null) {
      throw new ERR_INVALID_ARG_VALUE(name, value, modeDesc);
    }
    value = Number.parseInt(value, 8);
  }

  validateUint32(value, name);
  return value;
}

export function parseOpenFlags(
  flags: number | null | string,
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
