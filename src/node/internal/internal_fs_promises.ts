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

/* eslint-disable @typescript-eslint/require-await,@typescript-eslint/no-unused-vars */

import {
  getOptions,
  copyObject,
  normalizePath,
  kMaxUserId,
  validateCpOptions,
  toUnixTimestamp,
  validateRmdirOptions,
  type FilePath,
} from 'node-internal:internal_fs_utils';
import * as constants from 'node-internal:internal_fs_constants';
import {
  parseFileMode,
  validateInteger,
  validateBoolean,
  validateAbortSignal,
  validateOneOf,
} from 'node-internal:validators';
import type { RmDirOptions, CopyOptions } from 'node:fs';

export async function access(
  _path: FilePath,
  _mode: number = constants.F_OK
): Promise<void> {
  throw new Error('Not implemented');
}

export async function appendFile(
  _path: FilePath,
  _data: unknown,
  options: Record<string, unknown>
): Promise<void> {
  options = getOptions(options, { encoding: 'utf8', mode: 0o666, flag: 'a' });
  options = copyObject(options);
  options.flag ||= 'a';
  throw new Error('Not implemented');
}

export async function chmod(path: FilePath, mode: number): Promise<void> {
  path = normalizePath(path);
  mode = parseFileMode(mode, 'mode');
  throw new Error('Not implemented');
}

export async function chown(
  path: FilePath,
  uid: number,
  gid: number
): Promise<void> {
  path = normalizePath(path);
  validateInteger(uid, 'uid', -1, kMaxUserId);
  validateInteger(gid, 'gid', -1, kMaxUserId);
  throw new Error('Not implemented');
}

export async function copyFile(
  src: FilePath,
  dest: FilePath,
  _mode: number
): Promise<void> {
  src = normalizePath(src);
  dest = normalizePath(dest);
  throw new Error('Not implemented');
}

export async function cp(
  src: FilePath,
  dest: FilePath,
  options: CopyOptions
): Promise<void> {
  options = validateCpOptions(options);
  src = normalizePath(src);
  dest = normalizePath(dest);
  throw new Error('Not implemented');
}

export async function lchmod(_path: FilePath, _mode: number): Promise<void> {
  throw new Error('Not implemented');
}

export async function lchown(
  path: FilePath,
  uid: number,
  gid: number
): Promise<void> {
  path = normalizePath(path);
  validateInteger(uid, 'uid', -1, kMaxUserId);
  validateInteger(gid, 'gid', -1, kMaxUserId);
  throw new Error('Not implemented');
}

export async function lutimes(
  path: FilePath,
  atime: string | number | Date,
  mtime: string | number | Date
): Promise<void> {
  path = normalizePath(path);
  atime = toUnixTimestamp(atime);
  mtime = toUnixTimestamp(mtime);
  throw new Error('Not implemented');
}

export async function link(
  existingPath: FilePath,
  newPath: FilePath
): Promise<void> {
  existingPath = normalizePath(existingPath);
  newPath = normalizePath(newPath);
  throw new Error('Not implemented');
}

export async function lstat(
  path: FilePath,
  _options: Record<string, unknown> = { bigint: false }
): Promise<void> {
  path = normalizePath(path);
  throw new Error('Not implemented');
}

export async function mkdir(
  path: FilePath,
  options?: Record<string, unknown>
): Promise<void> {
  if (typeof options === 'number' || typeof options === 'string') {
    options = { mode: options };
  }
  const recursive = options?.recursive ?? false;
  let mode = options?.mode ?? 0o777;
  path = normalizePath(path);
  validateBoolean(recursive, 'options.recursive');
  mode = parseFileMode(mode, 'mode', 0o777);
  throw new Error('Not implemented');
}

export async function mkdtemp(
  prefix: FilePath,
  options: Record<string, unknown>
): Promise<void> {
  options = getOptions(options);
  prefix = normalizePath(prefix);
  throw new Error('Not implemented');
}

export async function open(
  path: FilePath,
  _flags: number | string | null,
  mode: number
): Promise<void> {
  path = normalizePath(path);
  mode = parseFileMode(mode, 'mode', 0o666);
  throw new Error('Not implemented');
}

export async function opendir(
  path: FilePath,
  options: Record<string, unknown>
): Promise<void> {
  path = normalizePath(path);
  options = getOptions(options, {
    encoding: 'utf8',
  });
  throw new Error('Not implemented');
}

export async function readdir(
  path: FilePath,
  options: Record<string, unknown>
): Promise<void> {
  options = copyObject(getOptions(options));
  path = normalizePath(path);
  throw new Error('Not implemented');
}

export async function readFile(
  _path: FilePath,
  options: Record<string, unknown>
): Promise<void> {
  options = getOptions(options, { flag: 'r' });
  throw new Error('Not implemented');
}

export async function readlink(
  path: FilePath,
  options: Record<string, unknown>
): Promise<void> {
  options = getOptions(options);
  path = normalizePath(path);
  throw new Error('Not implemented');
}

export async function realpath(
  path: FilePath,
  options: Record<string, unknown>
): Promise<void> {
  path = normalizePath(path);
  options = getOptions(options);
  throw new Error('Not implemented');
}

export async function rename(
  oldPath: FilePath,
  newPath: FilePath
): Promise<void> {
  oldPath = normalizePath(oldPath);
  newPath = normalizePath(newPath);
  throw new Error('Not implemented');
}

export async function rmdir(
  path: FilePath,
  options: RmDirOptions
): Promise<void> {
  path = normalizePath(path);
  options = validateRmdirOptions(options);
  throw new Error('Not implemented');
}

export async function rm(
  path: FilePath,
  _options: Record<string, unknown>
): Promise<void> {
  path = normalizePath(path);
  throw new Error('Not implemented');
}

export async function stat(
  path: FilePath,
  _options: Record<string, unknown> = { bigint: false }
): Promise<void> {
  path = normalizePath(path);
  throw new Error('Not implemented');
}

export async function statfs(
  _path: FilePath,
  _options: Record<string, unknown> = { bigint: false }
): Promise<void> {
  throw new Error('Not implemented');
}

export async function symlink(
  target: FilePath,
  path: FilePath,
  type: string | null | undefined
): Promise<void> {
  validateOneOf(type, 'type', ['dir', 'file', 'junction', null, undefined]);
  target = normalizePath(target);
  path = normalizePath(path);
  throw new Error('Not implemented');
}

export async function truncate(
  _path: FilePath,
  _len: number = 0
): Promise<void> {
  throw new Error('Not implemented');
}

export async function unlink(path: FilePath): Promise<void> {
  path = normalizePath(path);
  throw new Error('Not implemented');
}

export async function utimes(
  path: FilePath,
  atime: string | number | Date,
  mtime: string | number | Date
): Promise<void> {
  path = normalizePath(path);
  atime = toUnixTimestamp(atime);
  mtime = toUnixTimestamp(mtime);
  throw new Error('Not implemented');
}

export async function watch(): Promise<void> {
  throw new Error('Not implemented');
}

export async function writeFile(
  _path: FilePath,
  _data: unknown,
  options: Record<string, unknown>
): Promise<void> {
  options = getOptions(options, {
    encoding: 'utf8',
    mode: 0o666,
    flag: 'w',
    flush: false,
  });
  const flush = options.flush ?? false;

  validateBoolean(flush, 'options.flush');
  validateAbortSignal(options.signal, 'options.signal');

  throw new Error('Not implemented');
}
