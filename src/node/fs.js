// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// We don't currently implement the node:fs module. Instead we provide a stub that allows
// imports/require statements to work but throws an error if any of the functions are called.
/* eslint-disable */

import {
  default as promises,
  constants,
  makeUnsupportedFunction,
  makeUnsupportedClass,
} from 'node-internal:fs_promises';

export { promises, constants };

export const _toUnixTimestamp = makeUnsupportedFunction('_toUnixTimestamp');
export const access = makeUnsupportedFunction('access');
export const accessSync = makeUnsupportedFunction('accessSync');
export const appendFile = makeUnsupportedFunction('appendFile');
export const appendFileSync = makeUnsupportedFunction('appendFileSync');
export const chmod = makeUnsupportedFunction('chmod');
export const chmodSync = makeUnsupportedFunction('chmodSync');
export const chown = makeUnsupportedFunction('chown');
export const chownSync = makeUnsupportedFunction('chownSync');
export const close = makeUnsupportedFunction('close');
export const closeSync = makeUnsupportedFunction('closeSync');
export const copyFile = makeUnsupportedFunction('copyFile');
export const copyFileSync = makeUnsupportedFunction('copyFileSync');
export const cp = makeUnsupportedFunction('cp');
export const cpSync = makeUnsupportedFunction('cpSync');
export const createReadStream = makeUnsupportedFunction('createReadStream');
export const createWriteStream = makeUnsupportedFunction('createWriteStream');
export const exists = makeUnsupportedFunction('exists');
export const existsSync = makeUnsupportedFunction('existsSync');
export const fchmod = makeUnsupportedFunction('fchmod');
export const fchmodSync = makeUnsupportedFunction('fchmodSync');
export const fchown = makeUnsupportedFunction('fchown');
export const fchownSync = makeUnsupportedFunction('fchownSync');
export const fdatasync = makeUnsupportedFunction('fdatasync');
export const fdatasyncSync = makeUnsupportedFunction('fdatasyncSync');
export const fstat = makeUnsupportedFunction('fstat');
export const fstatSync = makeUnsupportedFunction('fstatSync');
export const fsync = makeUnsupportedFunction('fsync');
export const fsyncSync = makeUnsupportedFunction('fsyncSync');
export const ftruncate = makeUnsupportedFunction('ftruncate');
export const ftruncateSync = makeUnsupportedFunction('ftruncateSync');
export const futimes = makeUnsupportedFunction('futimes');
export const futimesSync = makeUnsupportedFunction('futimesSync');
export const lchmod = makeUnsupportedFunction('lchmod');
export const lchmodSync = makeUnsupportedFunction('lchmodSync');
export const lchown = makeUnsupportedFunction('lchown');
export const lchownSync = makeUnsupportedFunction('lchownSync');
export const link = makeUnsupportedFunction('link');
export const linkSync = makeUnsupportedFunction('linkSync');
export const lstat = makeUnsupportedFunction('lstat');
export const lstatSync = makeUnsupportedFunction('lstatSync');
export const lutimes = makeUnsupportedFunction('lutimes');
export const lutimesSync = makeUnsupportedFunction('lutimesSync');
export const mkdir = makeUnsupportedFunction('mkdir');
export const mkdirSync = makeUnsupportedFunction('mkdirSync');
export const mkdtemp = makeUnsupportedFunction('mkdtemp');
export const mkdtempSync = makeUnsupportedFunction('mkdtempSync');
export const open = makeUnsupportedFunction('open');
export const openAsBlob = makeUnsupportedFunction('openAsBlob');
export const openSync = makeUnsupportedFunction('openSync');
export const opendir = makeUnsupportedFunction('opendir');
export const opendirSync = makeUnsupportedFunction('opendirSync');
export const read = makeUnsupportedFunction('read');
export const readFile = makeUnsupportedFunction('readFile');
export const readFileSync = makeUnsupportedFunction('readFileSync');
export const readSync = makeUnsupportedFunction('readSync');
export const readdir = makeUnsupportedFunction('readdir');
export const readdirSync = makeUnsupportedFunction('readdirSync');
export const readlink = makeUnsupportedFunction('readlink');
export const readlinkSync = makeUnsupportedFunction('readlinkSync');
export const readv = makeUnsupportedFunction('readv');
export const readvSync = makeUnsupportedFunction('readvSync');
export const realpath = makeUnsupportedFunction('realpath');
export const realpathSync = makeUnsupportedFunction('realpathSync');
export const rename = makeUnsupportedFunction('rename');
export const renameSync = makeUnsupportedFunction('renameSync');
export const rm = makeUnsupportedFunction('rm');
export const rmSync = makeUnsupportedFunction('rmSync');
export const rmdir = makeUnsupportedFunction('rmdir');
export const rmdirSync = makeUnsupportedFunction('rmdirSync');
export const stat = makeUnsupportedFunction('stat');
export const statSync = makeUnsupportedFunction('statSync');
export const statfs = makeUnsupportedFunction('statfs');
export const statfsSync = makeUnsupportedFunction('statfsSync');
export const symlink = makeUnsupportedFunction('symlink');
export const symlinkSync = makeUnsupportedFunction('symlinkSync');
export const truncate = makeUnsupportedFunction('truncate');
export const truncateSync = makeUnsupportedFunction('truncateSync');
export const unlink = makeUnsupportedFunction('unlink');
export const unlinkSync = makeUnsupportedFunction('unlinkSync');
export const unwatchFile = makeUnsupportedFunction('unwatchFile');
export const utimes = makeUnsupportedFunction('utimes');
export const utimesSync = makeUnsupportedFunction('utimesSync');
export const watch = makeUnsupportedFunction('watch');
export const watchFile = makeUnsupportedFunction('watchFile');
export const write = makeUnsupportedFunction('write');
export const writeFile = makeUnsupportedFunction('writeFile');
export const writeFileSync = makeUnsupportedFunction('writeFileSync');
export const writeSync = makeUnsupportedFunction('writeSync');
export const writev = makeUnsupportedFunction('writev');
export const writevSync = makeUnsupportedFunction('writevSync');
export const Dir = makeUnsupportedClass('Dir');
export const Dirent = makeUnsupportedClass('Dirent');
export const FileReadStream = makeUnsupportedFunction('FileReadStream');
export const FileWriteStream = makeUnsupportedFunction('FileWriteStream');
export const ReadStream = makeUnsupportedFunction('ReadStream');
export const Stats = makeUnsupportedFunction('Stats');
export const WriteStream = makeUnsupportedFunction('WriteStream');

const exports = {
  _toUnixTimestamp,
  access,
  accessSync,
  appendFile,
  appendFileSync,
  chmod,
  chmodSync,
  chown,
  chownSync,
  close,
  closeSync,
  constants,
  promises,
  copyFile,
  copyFileSync,
  cp,
  cpSync,
  createReadStream,
  createWriteStream,
  exists,
  existsSync,
  fchmod,
  fchmodSync,
  fchown,
  fchownSync,
  fdatasync,
  fdatasyncSync,
  fstat,
  fstatSync,
  fsync,
  fsyncSync,
  ftruncate,
  ftruncateSync,
  futimes,
  futimesSync,
  lchmod,
  lchmodSync,
  lchown,
  lchownSync,
  link,
  linkSync,
  lstat,
  lstatSync,
  lutimes,
  lutimesSync,
  mkdir,
  mkdirSync,
  mkdtemp,
  mkdtempSync,
  open,
  openAsBlob,
  openSync,
  opendir,
  opendirSync,
  read,
  readFile,
  readFileSync,
  readSync,
  readdir,
  readdirSync,
  readlink,
  readlinkSync,
  readv,
  readvSync,
  realpath,
  realpathSync,
  rename,
  renameSync,
  rm,
  rmSync,
  rmdir,
  rmdirSync,
  stat,
  statSync,
  statfs,
  statfsSync,
  symlink,
  symlinkSync,
  truncate,
  truncateSync,
  unlink,
  unlinkSync,
  unwatchFile,
  utimes,
  utimesSync,
  watch,
  watchFile,
  write,
  writeFile,
  writeFileSync,
  writeSync,
  writev,
  writevSync,
  Dir,
  Dirent,
  FileReadStream,
  FileWriteStream,
  ReadStream,
  Stats,
  WriteStream,
};

Object.defineProperties(exports, {
  F_OK: { writable: false, configurable: false, value: undefined },
  R_OK: { writable: false, configurable: false, value: undefined },
  W_OK: { writable: false, configurable: false, value: undefined },
  X_OK: { writable: false, configurable: false, value: undefined },
});

export default exports;
