// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as promises from 'node-internal:internal_fs_promises';
import * as constants from 'node-internal:internal_fs_constants';
import * as callbackMethods from 'node-internal:internal_fs_callback';
import { Dirent, Dir } from 'node-internal:internal_fs';
import { Stats } from 'node-internal:internal_fs_utils';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';

export * from 'node-internal:internal_fs_callback';
import {
  ReadStream,
  WriteStream,
  createReadStream,
  createWriteStream,
} from 'node-internal:internal_fs_streams';
import {
  accessSync,
  existsSync,
  appendFileSync,
  chmodSync,
  chownSync,
  closeSync,
  copyFileSync,
  cpSync,
  fchmodSync,
  fchownSync,
  fdatasyncSync,
  fstatSync,
  fsyncSync,
  ftruncateSync,
  futimesSync,
  globSync,
  lchmodSync,
  lchownSync,
  lutimesSync,
  linkSync,
  lstatSync,
  mkdirSync,
  mkdtempSync,
  opendirSync,
  openSync,
  readdirSync,
  readFileSync,
  readlinkSync,
  readSync,
  readvSync,
  realpathSync,
  renameSync,
  rmdirSync,
  rmSync,
  statSync,
  statfsSync,
  symlinkSync,
  truncateSync,
  unlinkSync,
  utimesSync,
  writeFileSync,
  writeSync,
  writevSync,
  openAsBlob,
} from 'node-internal:internal_fs_sync';

const { F_OK, R_OK, W_OK, X_OK } = constants;

// Node.js exports these as aliases
export const FileWriteStream = WriteStream;
export const FileReadStream = ReadStream;

// fs.Utf8Stream is a writable stream for utf-8 text (Node >=24). We export
// the class for feature-detection parity but do not implement it.
export class Utf8Stream {
  constructor() {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Utf8Stream');
  }
}

export interface DisposableMkdtempSyncResult {
  path: string;
  remove(): void;
  [Symbol.dispose](): void;
}

// Node.js fs.mkdtempDisposableSync: mkdtempSync + an explicit-resource-mgmt
// wrapper that removes the directory on dispose.
export function mkdtempDisposableSync(
  prefix: Parameters<typeof mkdtempSync>[0],
  options?: Parameters<typeof mkdtempSync>[1]
): DisposableMkdtempSyncResult {
  const path = mkdtempSync(prefix, options);
  let removed = false;
  const remove = (): void => {
    if (removed) return;
    removed = true;
    rmSync(path, { recursive: true, force: true });
  };
  return {
    path,
    remove,
    [Symbol.dispose]: remove,
  };
}

export {
  constants,
  F_OK,
  R_OK,
  W_OK,
  X_OK,
  promises,
  Dirent,
  Dir,
  accessSync,
  existsSync,
  appendFileSync,
  chmodSync,
  chownSync,
  closeSync,
  copyFileSync,
  cpSync,
  fchmodSync,
  fchownSync,
  fdatasyncSync,
  fstatSync,
  fsyncSync,
  ftruncateSync,
  futimesSync,
  globSync,
  lchmodSync,
  lchownSync,
  lutimesSync,
  linkSync,
  lstatSync,
  mkdirSync,
  mkdtempSync,
  opendirSync,
  openSync,
  readdirSync,
  readFileSync,
  readlinkSync,
  readSync,
  readvSync,
  realpathSync,
  renameSync,
  rmdirSync,
  rmSync,
  statSync,
  statfsSync,
  symlinkSync,
  truncateSync,
  unlinkSync,
  utimesSync,
  writeFileSync,
  writeSync,
  writevSync,
  Stats,
  ReadStream,
  WriteStream,
  createReadStream,
  createWriteStream,
  openAsBlob,
};

export default {
  constants,
  F_OK,
  R_OK,
  W_OK,
  X_OK,
  promises,
  Dirent,
  Dir,
  Stats,
  ...callbackMethods,
  accessSync,
  existsSync,
  appendFileSync,
  chmodSync,
  chownSync,
  closeSync,
  copyFileSync,
  cpSync,
  fchmodSync,
  fchownSync,
  fdatasyncSync,
  fstatSync,
  fsyncSync,
  ftruncateSync,
  futimesSync,
  globSync,
  lchmodSync,
  lchownSync,
  lutimesSync,
  linkSync,
  lstatSync,
  mkdirSync,
  mkdtempSync,
  opendirSync,
  openSync,
  readdirSync,
  readFileSync,
  readlinkSync,
  readSync,
  readvSync,
  realpathSync,
  renameSync,
  rmdirSync,
  rmSync,
  statSync,
  statfsSync,
  symlinkSync,
  truncateSync,
  unlinkSync,
  utimesSync,
  writeFileSync,
  writeSync,
  writevSync,
  WriteStream,
  ReadStream,
  FileWriteStream,
  FileReadStream,
  createReadStream,
  createWriteStream,
  openAsBlob,
  Utf8Stream,
  mkdtempDisposableSync,
};
