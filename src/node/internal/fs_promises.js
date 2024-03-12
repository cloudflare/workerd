// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
/* eslint-disable */

export function makeUnsupportedFunction(name, pfx = '') {
  return function() {
    throw new Error(`node:fs${pfx} ${name} is not implemented`);
  };
}

export function makeUnsupportedClass(name, pfx = '') {
  return class {
    constructor() {
      throw new Error(`node:fs${pfx}.${name} is not implemented`);
    }
  };
}

export const constants = {};

Object.defineProperties(constants, {
  COPYFILE_EXCL: { writable: false, configurable: false, value: undefined},
  COPYFILE_FICLONE: { writable: false, configurable: false, value: undefined},
  COPYFILE_FICLONE_FORCE: { writable: false, configurable: false, value: undefined},
  EXTENSIONLESS_FORMAT_JAVASCRIPT: { writable: false, configurable: false, value: undefined},
  EXTENSIONLESS_FORMAT_WASM: { writable: false, configurable: false, value: undefined},
  F_OK: { writable: false, configurable: false, value: undefined},
  O_APPEND: { writable: false, configurable: false, value: undefined},
  O_CREAT: { writable: false, configurable: false, value: undefined},
  O_DIRECT: { writable: false, configurable: false, value: undefined},
  O_DIRECTORY: { writable: false, configurable: false, value: undefined},
  O_DSYNC: { writable: false, configurable: false, value: undefined},
  O_EXCL: { writable: false, configurable: false, value: undefined},
  O_NOATIME: { writable: false, configurable: false, value: undefined},
  O_NOCTTY: { writable: false, configurable: false, value: undefined},
  O_NOFOLLOW: { writable: false, configurable: false, value: undefined},
  O_NONBLOCK: { writable: false, configurable: false, value: undefined},
  O_RDONLY: { writable: false, configurable: false, value: undefined},
  O_RDWR: { writable: false, configurable: false, value: undefined},
  O_SYNC: { writable: false, configurable: false, value: undefined},
  O_TRUNC: { writable: false, configurable: false, value: undefined},
  O_WRONLY: { writable: false, configurable: false, value: undefined},
  R_OK: { writable: false, configurable: false, value: undefined},
  S_IFBLK: { writable: false, configurable: false, value: undefined},
  S_IFCHR: { writable: false, configurable: false, value: undefined},
  S_IFDIR: { writable: false, configurable: false, value: undefined},
  S_IFIFO: { writable: false, configurable: false, value: undefined},
  S_IFLNK: { writable: false, configurable: false, value: undefined},
  S_IFMT: { writable: false, configurable: false, value: undefined},
  S_IFREG: { writable: false, configurable: false, value: undefined},
  S_IFSOCK: { writable: false, configurable: false, value: undefined},
  S_IRGRP: { writable: false, configurable: false, value: undefined},
  S_IROTH: { writable: false, configurable: false, value: undefined},
  S_IRUSR: { writable: false, configurable: false, value: undefined},
  S_IRWXG: { writable: false, configurable: false, value: undefined},
  S_IRWXO: { writable: false, configurable: false, value: undefined},
  S_IRWXU: { writable: false, configurable: false, value: undefined},
  S_IWGRP: { writable: false, configurable: false, value: undefined},
  S_IWOTH: { writable: false, configurable: false, value: undefined},
  S_IWUSR: { writable: false, configurable: false, value: undefined},
  S_IXGRP: { writable: false, configurable: false, value: undefined},
  S_IXOTH: { writable: false, configurable: false, value: undefined},
  S_IXUSR: { writable: false, configurable: false, value: undefined},
  UV_DIRENT_BLOCK: { writable: false, configurable: false, value: undefined},
  UV_DIRENT_CHAR: { writable: false, configurable: false, value: undefined},
  UV_DIRENT_DIR: { writable: false, configurable: false, value: undefined},
  UV_DIRENT_FIFO: { writable: false, configurable: false, value: undefined},
  UV_DIRENT_FILE: { writable: false, configurable: false, value: undefined},
  UV_DIRENT_LINK: { writable: false, configurable: false, value: undefined},
  UV_DIRENT_SOCKET: { writable: false, configurable: false, value: undefined},
  UV_DIRENT_UNKNOWN: { writable: false, configurable: false, value: undefined},
  UV_FS_COPYFILE_EXCL: { writable: false, configurable: false, value: undefined},
  UV_FS_COPYFILE_FICLONE: { writable: false, configurable: false, value: undefined},
  UV_FS_COPYFILE_FICLONE_FORCE: { writable: false, configurable: false, value: undefined},
  UV_FS_O_FILEMAP: { writable: false, configurable: false, value: undefined},
  UV_FS_SYMLINK_DIR: { writable: false, configurable: false, value: undefined},
  UV_FS_SYMLINK_JUNCTION: { writable: false, configurable: false, value: undefined},
  W_OK: { writable: false, configurable: false, value: undefined},
  X_OK: { writable: false, configurable: false, value: undefined},
});

export const access = makeUnsupportedFunction('access', '/promises');
export const appendFile = makeUnsupportedFunction('appendFile', '/promises');
export const chmod = makeUnsupportedFunction('chmod', '/promises');
export const chown = makeUnsupportedFunction('chown', '/promises');
export const copyFile = makeUnsupportedFunction('copyFile', '/promises');
export const cp = makeUnsupportedFunction('cp', '/promises');
export const lchmod = makeUnsupportedFunction('lchmod', '/promises');
export const lchown = makeUnsupportedFunction('lchown', '/promises');
export const lutimes = makeUnsupportedFunction('lutimes', '/promises');
export const link = makeUnsupportedFunction('link', '/promises');
export const lstat = makeUnsupportedFunction('lstat', '/promises');
export const mkdir = makeUnsupportedFunction('mkdir', '/promises');
export const mkdtemp = makeUnsupportedFunction('mkdtemp', '/promises');
export const open = makeUnsupportedFunction('open', '/promises');
export const opendir = makeUnsupportedFunction('opendir', '/promises');
export const readdir = makeUnsupportedFunction('readdir', '/promises');
export const readFile = makeUnsupportedFunction('readFile', '/promises');
export const readlink = makeUnsupportedFunction('readlink', '/promises');
export const realpath = makeUnsupportedFunction('realpath', '/promises');
export const rename = makeUnsupportedFunction('rename', '/promises');
export const rmdir = makeUnsupportedFunction('rmdir', '/promises');
export const rm = makeUnsupportedFunction('rm', '/promises');
export const stat = makeUnsupportedFunction('stat', '/promises');
export const statfs = makeUnsupportedFunction('statfs', '/promises');
export const symlink = makeUnsupportedFunction('symlink', '/promises');
export const truncate = makeUnsupportedFunction('truncate', '/promises');
export const unlink = makeUnsupportedFunction('unlink', '/promises');
export const utimes = makeUnsupportedFunction('utimes', '/promises');
export const watch = makeUnsupportedFunction('watch', '/promises');
export const writeFile = makeUnsupportedFunction('writeFile', '/promises');
export const FileHandle = makeUnsupportedClass('FileHandle', '/promises');

const exports = {
  access,
  appendFile,
  chmod,
  chown,
  copyFile,
  cp,
  lchmod,
  lchown,
  lutimes,
  link,
  lstat,
  mkdir,
  mkdtemp,
  open,
  opendir,
  readdir,
  readFile,
  readlink,
  realpath,
  rename,
  rmdir,
  rm,
  stat,
  statfs,
  symlink,
  truncate,
  unlink,
  utimes,
  watch,
  writeFile,
  FileHandle,
};

export default exports;
