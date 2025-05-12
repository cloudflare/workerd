export const F_OK = 0;
export const X_OK = 1;
export const W_OK = 2;
export const R_OK = 4;
export const S_IFMT = 61440;
export const S_IFREG = 32768;
export const S_IFDIR = 16384;
export const S_IFCHR = 8192;
export const S_IFBLK = 24576;
export const S_IFIFO = 4096;
export const S_IFLNK = 40960;
export const S_IFSOCK = 49152;

export const O_APPEND = 1024;
export const O_CREAT = 64;
export const O_DIRECT = 16384;
export const O_DIRECTORY = 65536;
export const O_DSYNC = 4096;
export const O_EXCL = 128;
export const O_NOATIME = 262144;
export const O_NOCTTY = 256;
export const O_NOFOLLOW = 8192;
export const O_NONBLOCK = 2048;
export const O_RDONLY = 0;
export const O_RDWR = 2;
export const O_SYNC = 128;
export const O_TRUNC = 512;
export const O_WRONLY = 1;

export const UV_DIRENT_UNKNOWN = 0;
export const UV_DIRENT_FILE = 1;
export const UV_DIRENT_DIR = 2;
export const UV_DIRENT_LINK = 3;
export const UV_DIRENT_FIFO = 4;
export const UV_DIRENT_SOCKET = 5;
export const UV_DIRENT_CHR = 6;
export const UV_DIRENT_BLOCK = 7;

export const COPYFILE_EXCL = 1;
export const COPYFILE_FICLONE = 2;
export const COPYFILE_FICLONE_FORCE = 4;

export const kMaxUserId = 2 ** 32 - 1;

export default {
  F_OK,
  X_OK,
  W_OK,
  R_OK,
  S_IFMT,
  S_IFREG,
  S_IFDIR,
  S_IFCHR,
  S_IFBLK,
  S_IFIFO,
  S_IFLNK,
  S_IFSOCK,
  O_APPEND,
  O_CREAT,
  O_DIRECT,
  O_DIRECTORY,
  O_DSYNC,
  O_EXCL,
  O_NOATIME,
  O_NOCTTY,
  O_NOFOLLOW,
  O_NONBLOCK,
  O_RDONLY,
  O_RDWR,
  O_SYNC,
  O_TRUNC,
  O_WRONLY,
  UV_DIRENT_UNKNOWN,
  UV_DIRENT_FILE,
  UV_DIRENT_DIR,
  UV_DIRENT_LINK,
  UV_DIRENT_FIFO,
  UV_DIRENT_SOCKET,
  UV_DIRENT_CHR,
  UV_DIRENT_BLOCK,
  COPYFILE_EXCL,
  COPYFILE_FICLONE,
  COPYFILE_FICLONE_FORCE,
};
