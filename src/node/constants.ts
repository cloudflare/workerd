// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
export const RTLD_LAZY = 1;
export const RTLD_NOW = 2;
export const RTLD_GLOBAL = 8;
export const RTLD_LOCAL = 4;
export const E2BIG = 7;
export const EACCES = 13;
export const EADDRINUSE = 48;
export const EADDRNOTAVAIL = 49;
export const EAFNOSUPPORT = 47;
export const EAGAIN = 35;
export const EALREADY = 37;
export const EBADF = 9;
export const EBADMSG = 94;
export const EBUSY = 16;
export const ECANCELED = 89;
export const ECHILD = 10;
export const ECONNABORTED = 53;
export const ECONNREFUSED = 61;
export const ECONNRESET = 54;
export const EDEADLK = 11;
export const EDESTADDRREQ = 39;
export const EDOM = 33;
export const EDQUOT = 69;
export const EEXIST = 17;
export const EFAULT = 14;
export const EFBIG = 27;
export const EHOSTUNREACH = 65;
export const EIDRM = 90;
export const EILSEQ = 92;
export const EINPROGRESS = 36;
export const EINTR = 4;
export const EINVAL = 22;
export const EIO = 5;
export const EISCONN = 56;
export const EISDIR = 21;
export const ELOOP = 62;
export const EMFILE = 24;
export const EMLINK = 31;
export const EMSGSIZE = 40;
export const EMULTIHOP = 95;
export const ENAMETOOLONG = 63;
export const ENETDOWN = 50;
export const ENETRESET = 52;
export const ENETUNREACH = 51;
export const ENFILE = 23;
export const ENOBUFS = 55;
export const ENODATA = 96;
export const ENODEV = 19;
export const ENOENT = 2;
export const ENOEXEC = 8;
export const ENOLCK = 77;
export const ENOLINK = 97;
export const ENOMEM = 12;
export const ENOMSG = 91;
export const ENOPROTOOPT = 42;
export const ENOSPC = 28;
export const ENOSR = 98;
export const ENOSTR = 99;
export const ENOSYS = 78;
export const ENOTCONN = 57;
export const ENOTDIR = 20;
export const ENOTEMPTY = 66;
export const ENOTSOCK = 38;
export const ENOTSUP = 45;
export const ENOTTY = 25;
export const ENXIO = 6;
export const EOPNOTSUPP = 102;
export const EOVERFLOW = 84;
export const EPERM = 1;
export const EPIPE = 32;
export const EPROTO = 100;
export const EPROTONOSUPPORT = 43;
export const EPROTOTYPE = 41;
export const ERANGE = 34;
export const EROFS = 30;
export const ESPIPE = 29;
export const ESRCH = 3;
export const ESTALE = 70;
export const ETIME = 101;
export const ETIMEDOUT = 60;
export const ETXTBSY = 26;
export const EWOULDBLOCK = 35;
export const EXDEV = 18;
export const PRIORITY_LOW = 19;
export const PRIORITY_BELOW_NORMAL = 10;
export const PRIORITY_NORMAL = 0;
export const PRIORITY_ABOVE_NORMAL = -7;
export const PRIORITY_HIGH = -14;
export const PRIORITY_HIGHEST = -20;
export const SIGHUP = 1;
export const SIGINT = 2;
export const SIGQUIT = 3;
export const SIGILL = 4;
export const SIGTRAP = 5;
export const SIGABRT = 6;
export const SIGIOT = 6;
export const SIGBUS = 10;
export const SIGFPE = 8;
export const SIGKILL = 9;
export const SIGUSR1 = 30;
export const SIGSEGV = 11;
export const SIGUSR2 = 31;
export const SIGPIPE = 13;
export const SIGALRM = 14;
export const SIGTERM = 15;
export const SIGCHLD = 20;
export const SIGCONT = 19;
export const SIGSTOP = 17;
export const SIGTSTP = 18;
export const SIGTTIN = 21;
export const SIGTTOU = 22;
export const SIGURG = 16;
export const SIGXCPU = 24;
export const SIGXFSZ = 25;
export const SIGVTALRM = 26;
export const SIGPROF = 27;
export const SIGWINCH = 28;
export const SIGIO = 23;
export const SIGINFO = 29;
export const SIGSYS = 12;
export const UV_FS_SYMLINK_DIR = 1;
export const UV_FS_SYMLINK_JUNCTION = 2;
export const O_RDONLY = 0;
export const O_WRONLY = 1;
export const O_RDWR = 2;
export const UV_DIRENT_UNKNOWN = 0;
export const UV_DIRENT_FILE = 1;
export const UV_DIRENT_DIR = 2;
export const UV_DIRENT_LINK = 3;
export const UV_DIRENT_FIFO = 4;
export const UV_DIRENT_SOCKET = 5;
export const UV_DIRENT_CHAR = 6;
export const UV_DIRENT_BLOCK = 7;
export const S_IFMT = 61440;
export const S_IFREG = 32768;
export const S_IFDIR = 16384;
export const S_IFCHR = 8192;
export const S_IFBLK = 24576;
export const S_IFIFO = 4096;
export const S_IFLNK = 40960;
export const S_IFSOCK = 49152;
export const O_CREAT = 512;
export const O_EXCL = 2048;
export const UV_FS_O_FILEMAP = 0;
export const O_NOCTTY = 131072;
export const O_TRUNC = 1024;
export const O_APPEND = 8;
export const O_DIRECTORY = 1048576;
export const O_NOFOLLOW = 256;
export const O_SYNC = 128;
export const O_DSYNC = 4194304;
export const O_SYMLINK = 2097152;
export const O_NONBLOCK = 4;
export const S_IRWXU = 448;
export const S_IRUSR = 256;
export const S_IWUSR = 128;
export const S_IXUSR = 64;
export const S_IRWXG = 56;
export const S_IRGRP = 32;
export const S_IWGRP = 16;
export const S_IXGRP = 8;
export const S_IRWXO = 7;
export const S_IROTH = 4;
export const S_IWOTH = 2;
export const S_IXOTH = 1;
export const F_OK = 0;
export const R_OK = 4;
export const W_OK = 2;
export const X_OK = 1;
export const UV_FS_COPYFILE_EXCL = 1;
export const COPYFILE_EXCL = 1;
export const UV_FS_COPYFILE_FICLONE = 2;
export const COPYFILE_FICLONE = 2;
export const UV_FS_COPYFILE_FICLONE_FORCE = 4;
export const COPYFILE_FICLONE_FORCE = 4;
export const OPENSSL_VERSION_NUMBER = 805306624;
export const SSL_OP_ALL = 2147485776;
export const SSL_OP_ALLOW_NO_DHE_KEX = 1024;
export const SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION = 262144;
export const SSL_OP_CIPHER_SERVER_PREFERENCE = 4194304;
export const SSL_OP_CISCO_ANYCONNECT = 32768;
export const SSL_OP_COOKIE_EXCHANGE = 8192;
export const SSL_OP_CRYPTOPRO_TLSEXT_BUG = 2147483648;
export const SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS = 2048;
export const SSL_OP_LEGACY_SERVER_CONNECT = 4;
export const SSL_OP_NO_COMPRESSION = 131072;
export const SSL_OP_NO_ENCRYPT_THEN_MAC = 524288;
export const SSL_OP_NO_QUERY_MTU = 4096;
export const SSL_OP_NO_RENEGOTIATION = 1073741824;
export const SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION = 65536;
export const SSL_OP_NO_SSLv2 = 0;
export const SSL_OP_NO_SSLv3 = 33554432;
export const SSL_OP_NO_TICKET = 16384;
export const SSL_OP_NO_TLSv1 = 67108864;
export const SSL_OP_NO_TLSv1_1 = 268435456;
export const SSL_OP_NO_TLSv1_2 = 134217728;
export const SSL_OP_NO_TLSv1_3 = 536870912;
export const SSL_OP_PRIORITIZE_CHACHA = 2097152;
export const SSL_OP_TLS_ROLLBACK_BUG = 8388608;
export const ENGINE_METHOD_RSA = 1;
export const ENGINE_METHOD_DSA = 2;
export const ENGINE_METHOD_DH = 4;
export const ENGINE_METHOD_RAND = 8;
export const ENGINE_METHOD_EC = 2048;
export const ENGINE_METHOD_CIPHERS = 64;
export const ENGINE_METHOD_DIGESTS = 128;
export const ENGINE_METHOD_PKEY_METHS = 512;
export const ENGINE_METHOD_PKEY_ASN1_METHS = 1024;
export const ENGINE_METHOD_ALL = 65535;
export const ENGINE_METHOD_NONE = 0;
export const DH_CHECK_P_NOT_SAFE_PRIME = 2;
export const DH_CHECK_P_NOT_PRIME = 1;
export const DH_UNABLE_TO_CHECK_GENERATOR = 4;
export const DH_NOT_SUITABLE_GENERATOR = 8;
export const RSA_PKCS1_PADDING = 1;
export const RSA_NO_PADDING = 3;
export const RSA_PKCS1_OAEP_PADDING = 4;
export const RSA_X931_PADDING = 5;
export const RSA_PKCS1_PSS_PADDING = 6;
export const RSA_PSS_SALTLEN_DIGEST = -1;
export const RSA_PSS_SALTLEN_MAX_SIGN = -2;
export const RSA_PSS_SALTLEN_AUTO = -2;
export const defaultCoreCipherList = '';
export const TLS1_VERSION = 769;
export const TLS1_1_VERSION = 770;
export const TLS1_2_VERSION = 771;
export const TLS1_3_VERSION = 772;
export const POINT_CONVERSION_COMPRESSED = 2;
export const POINT_CONVERSION_UNCOMPRESSED = 4;
export const POINT_CONVERSION_HYBRID = 6;

const constants = {
  RTLD_LAZY,
  RTLD_NOW,
  RTLD_GLOBAL,
  RTLD_LOCAL,
  E2BIG,
  EACCES,
  EADDRINUSE,
  EADDRNOTAVAIL,
  EAFNOSUPPORT,
  EAGAIN,
  EALREADY,
  EBADF,
  EBADMSG,
  EBUSY,
  ECANCELED,
  ECHILD,
  ECONNABORTED,
  ECONNREFUSED,
  ECONNRESET,
  EDEADLK,
  EDESTADDRREQ,
  EDOM,
  EDQUOT,
  EEXIST,
  EFAULT,
  EFBIG,
  EHOSTUNREACH,
  EIDRM,
  EILSEQ,
  EINPROGRESS,
  EINTR,
  EINVAL,
  EIO,
  EISCONN,
  EISDIR,
  ELOOP,
  EMFILE,
  EMLINK,
  EMSGSIZE,
  EMULTIHOP,
  ENAMETOOLONG,
  ENETDOWN,
  ENETRESET,
  ENETUNREACH,
  ENFILE,
  ENOBUFS,
  ENODATA,
  ENODEV,
  ENOENT,
  ENOEXEC,
  ENOLCK,
  ENOLINK,
  ENOMEM,
  ENOMSG,
  ENOPROTOOPT,
  ENOSPC,
  ENOSR,
  ENOSTR,
  ENOSYS,
  ENOTCONN,
  ENOTDIR,
  ENOTEMPTY,
  ENOTSOCK,
  ENOTSUP,
  ENOTTY,
  ENXIO,
  EOPNOTSUPP,
  EOVERFLOW,
  EPERM,
  EPIPE,
  EPROTO,
  EPROTONOSUPPORT,
  EPROTOTYPE,
  ERANGE,
  EROFS,
  ESPIPE,
  ESRCH,
  ESTALE,
  ETIME,
  ETIMEDOUT,
  ETXTBSY,
  EWOULDBLOCK,
  EXDEV,
  PRIORITY_LOW,
  PRIORITY_BELOW_NORMAL,
  PRIORITY_NORMAL,
  PRIORITY_ABOVE_NORMAL,
  PRIORITY_HIGH,
  PRIORITY_HIGHEST,
  SIGHUP,
  SIGINT,
  SIGQUIT,
  SIGILL,
  SIGTRAP,
  SIGABRT,
  SIGIOT,
  SIGBUS,
  SIGFPE,
  SIGKILL,
  SIGUSR1,
  SIGSEGV,
  SIGUSR2,
  SIGPIPE,
  SIGALRM,
  SIGTERM,
  SIGCHLD,
  SIGCONT,
  SIGSTOP,
  SIGTSTP,
  SIGTTIN,
  SIGTTOU,
  SIGURG,
  SIGXCPU,
  SIGXFSZ,
  SIGVTALRM,
  SIGPROF,
  SIGWINCH,
  SIGIO,
  SIGINFO,
  SIGSYS,
  UV_FS_SYMLINK_DIR,
  UV_FS_SYMLINK_JUNCTION,
  O_RDONLY,
  O_WRONLY,
  O_RDWR,
  UV_DIRENT_UNKNOWN,
  UV_DIRENT_FILE,
  UV_DIRENT_DIR,
  UV_DIRENT_LINK,
  UV_DIRENT_FIFO,
  UV_DIRENT_SOCKET,
  UV_DIRENT_CHAR,
  UV_DIRENT_BLOCK,
  S_IFMT,
  S_IFREG,
  S_IFDIR,
  S_IFCHR,
  S_IFBLK,
  S_IFIFO,
  S_IFLNK,
  S_IFSOCK,
  O_CREAT,
  O_EXCL,
  UV_FS_O_FILEMAP,
  O_NOCTTY,
  O_TRUNC,
  O_APPEND,
  O_DIRECTORY,
  O_NOFOLLOW,
  O_SYNC,
  O_DSYNC,
  O_SYMLINK,
  O_NONBLOCK,
  S_IRWXU,
  S_IRUSR,
  S_IWUSR,
  S_IXUSR,
  S_IRWXG,
  S_IRGRP,
  S_IWGRP,
  S_IXGRP,
  S_IRWXO,
  S_IROTH,
  S_IWOTH,
  S_IXOTH,
  F_OK,
  R_OK,
  W_OK,
  X_OK,
  UV_FS_COPYFILE_EXCL,
  COPYFILE_EXCL,
  UV_FS_COPYFILE_FICLONE,
  COPYFILE_FICLONE,
  UV_FS_COPYFILE_FICLONE_FORCE,
  COPYFILE_FICLONE_FORCE,
  OPENSSL_VERSION_NUMBER,
  SSL_OP_ALL,
  SSL_OP_ALLOW_NO_DHE_KEX,
  SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION,
  SSL_OP_CIPHER_SERVER_PREFERENCE,
  SSL_OP_CISCO_ANYCONNECT,
  SSL_OP_COOKIE_EXCHANGE,
  SSL_OP_CRYPTOPRO_TLSEXT_BUG,
  SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS,
  SSL_OP_LEGACY_SERVER_CONNECT,
  SSL_OP_NO_COMPRESSION,
  SSL_OP_NO_ENCRYPT_THEN_MAC,
  SSL_OP_NO_QUERY_MTU,
  SSL_OP_NO_RENEGOTIATION,
  SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION,
  SSL_OP_NO_SSLv2,
  SSL_OP_NO_SSLv3,
  SSL_OP_NO_TICKET,
  SSL_OP_NO_TLSv1,
  SSL_OP_NO_TLSv1_1,
  SSL_OP_NO_TLSv1_2,
  SSL_OP_NO_TLSv1_3,
  SSL_OP_PRIORITIZE_CHACHA,
  SSL_OP_TLS_ROLLBACK_BUG,
  ENGINE_METHOD_RSA,
  ENGINE_METHOD_DSA,
  ENGINE_METHOD_DH,
  ENGINE_METHOD_RAND,
  ENGINE_METHOD_EC,
  ENGINE_METHOD_CIPHERS,
  ENGINE_METHOD_DIGESTS,
  ENGINE_METHOD_PKEY_METHS,
  ENGINE_METHOD_PKEY_ASN1_METHS,
  ENGINE_METHOD_ALL,
  ENGINE_METHOD_NONE,
  DH_CHECK_P_NOT_SAFE_PRIME,
  DH_CHECK_P_NOT_PRIME,
  DH_UNABLE_TO_CHECK_GENERATOR,
  DH_NOT_SUITABLE_GENERATOR,
  RSA_PKCS1_PADDING,
  RSA_NO_PADDING,
  RSA_PKCS1_OAEP_PADDING,
  RSA_X931_PADDING,
  RSA_PKCS1_PSS_PADDING,
  RSA_PSS_SALTLEN_DIGEST,
  RSA_PSS_SALTLEN_MAX_SIGN,
  RSA_PSS_SALTLEN_AUTO,
  defaultCoreCipherList,
  TLS1_VERSION,
  TLS1_1_VERSION,
  TLS1_2_VERSION,
  TLS1_3_VERSION,
  POINT_CONVERSION_COMPRESSED,
  POINT_CONVERSION_UNCOMPRESSED,
  POINT_CONVERSION_HYBRID,
};
const keys = Object.keys(constants);
for (const key of keys) {
  Object.defineProperty(constants, key, { writable: false });
}

export default constants;
