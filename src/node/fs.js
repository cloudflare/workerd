// We don't currently implement the node:fs module. Instead we provide a stub that allows
// imports/require statements to work but throws an error if any of the functions are called.

const constants = {};

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

function makeUnsupportedFunction(name) {
  return function() {
    throw new Error(`node:fs ${name} is not implemented`);
  };
}

function makeUnsupportedClass(name) {
  return class {
    public constructor() {
      throw new Error(`node:fs.${name} is not implemented`);
    }
  };
}

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
export const promises = makeUnsupportedFunction('promises');
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
  promises,
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
