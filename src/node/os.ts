// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Exceedingly minimal implementation of the `node:os` module for Workers.
// Generally all stubs that return empty or default values since we don't
// ever expose any actual OS information to the user.

import { validateNumber, validateObject } from 'node-internal:validators';
import { Buffer } from 'node-internal:internal_buffer';
import { ERR_INVALID_ARG_VALUE } from 'node-internal:internal_errors';
import type { ValidEncoding } from 'node-internal:internal_fs_utils';
import type { UserInfo, CpuInfo, NetworkInterfaceInfo } from 'node:os';

// We always assume POSIX in the workers environment
export const EOL = '\n';
export const devNull = '/dev/null';

export function availableParallelism(): number {
  return 1;
}

// While Workers does support arm in production, that fact is not exposed
// to the user. Let's just always report x64.
export function arch(): string {
  return 'x64';
}

// We do not expose CPU information to Workers.
export function cpus(): CpuInfo[] {
  return [];
}

export function endianness(): 'LE' | 'BE' {
  return 'LE';
}

export function freemem(): number {
  // We do not currently expose memory information to Workers. We might
  // be able to in the future.
  return 0;
}

export function getPriority(pid?: number): number {
  // Workers does not support process priority.
  if (pid !== undefined) validateNumber(pid, 'pid');
  return 0;
}

export function homedir(): string {
  // Workers really does not have a home directory. Return /tmp
  // in preparation for the availability of node:fs in the future.
  return '/tmp/';
}

export function hostname(): string {
  // Workers does not have a hostname. Return 'localhost' for compatibility.
  return 'localhost';
}

export function loadavg(): number[] {
  // Workers does not expose load average information.
  return [0, 0, 0];
}

export function machine(): string {
  // Workers does not expose the machine architecture.
  return 'x86_64';
}

export function networkInterfaces(): NetworkInterfaceInfo[] {
  // Workers does not expose network interfaces.
  // @ts-expect-error TS2740 We don't export properties here.
  return {};
}

export function platform(): NodeJS.Platform {
  // Workers only supports POSIX platforms.
  return 'linux';
}

export function release(): string {
  // Workers does not expose the OS release information.
  return '';
}

export function setPriority(pid: number, priority: number): void {
  // Workers does not support process priority.
  // Just ignore.
  validateNumber(pid, 'pid');
  validateNumber(priority, 'priority');
}

export function tmpdir(): string {
  return '/tmp/';
}

export function totalmem(): number {
  // We do not currently expose memory information to Workers. We might
  // be able to in the future.
  return 0;
}

export function type(): string {
  return 'Linux';
}

export function uptime(): number {
  // For now, we do not report uptime in Workers as it is not clear
  // exactly what it should be. We might be able to in the future
  // but we'll need to define what it means first.
  return 0;
}

export function userInfo(
  options: { encoding?: ValidEncoding | undefined } = {}
): UserInfo<unknown> {
  // We really do not have user information to share.
  validateObject(options, 'options');
  const { encoding = null } = options;
  if (
    encoding !== null &&
    encoding !== 'buffer' &&
    !Buffer.isEncoding(encoding)
  ) {
    throw new ERR_INVALID_ARG_VALUE(
      'options.encoding',
      encoding,
      'must be a valid encoding'
    );
  }
  // @ts-expect-error TS2739 We don't export properties here.
  return {};
}

export function version(): string {
  // Workers does not expose the OS version.
  return '';
}

export const constants = {
  __proto__: null,
  UV_UDP_REUSEADDR: 4,
  dlopen: {
    __proto__: null,
    RTLD_LAZY: 1,
    RTLD_NOW: 2,
    RTLD_GLOBAL: 256,
    RTLD_LOCAL: 0,
    RTLD_DEEPBIND: 8,
  },
  errno: {
    __proto__: null,
    E2BIG: 7,
    EACCES: 13,
    EADDRINUSE: 98,
    EADDRNOTAVAIL: 99,
    EAFNOSUPPORT: 97,
    EAGAIN: 11,
    EALREADY: 114,
    EBADF: 9,
    EBADMSG: 74,
    EBUSY: 16,
    ECANCELED: 125,
    ECHILD: 10,
    ECONNABORTED: 103,
    ECONNREFUSED: 111,
    ECONNRESET: 104,
    EDEADLK: 35,
    EDESTADDRREQ: 89,
    EDOM: 33,
    EDQUOT: 122,
    EEXIST: 17,
    EFAULT: 14,
    EFBIG: 27,
    EHOSTUNREACH: 113,
    EIDRM: 43,
    EILSEQ: 84,
    EINPROGRESS: 115,
    EINTR: 4,
    EINVAL: 22,
    EIO: 5,
    EISCONN: 106,
    EISDIR: 21,
    ELOOP: 40,
    EMFILE: 24,
    EMLINK: 31,
    EMSGSIZE: 90,
    EMULTIHOP: 72,
    ENAMETOOLONG: 36,
    ENETDOWN: 100,
    ENETRESET: 102,
    ENETUNREACH: 101,
    ENFILE: 23,
    ENOBUFS: 105,
    ENODATA: 61,
    ENODEV: 19,
    ENOENT: 2,
    ENOEXEC: 8,
    ENOLCK: 37,
    ENOLINK: 67,
    ENOMEM: 12,
    ENOMSG: 42,
    ENOPROTOOPT: 92,
    ENOSPC: 28,
    ENOSR: 63,
    ENOSTR: 60,
    ENOSYS: 38,
    ENOTCONN: 107,
    ENOTDIR: 20,
    ENOTEMPTY: 39,
    ENOTSOCK: 88,
    ENOTSUP: 95,
    ENOTTY: 25,
    ENXIO: 6,
    EOPNOTSUPP: 95,
    EOVERFLOW: 75,
    EPERM: 1,
    EPIPE: 32,
    EPROTO: 71,
    EPROTONOSUPPORT: 93,
    EPROTOTYPE: 91,
    ERANGE: 34,
    EROFS: 30,
    ESPIPE: 29,
    ESRCH: 3,
    ESTALE: 116,
    ETIME: 62,
    ETIMEDOUT: 110,
    ETXTBSY: 26,
    EWOULDBLOCK: 11,
    EXDEV: 18,
  },
  signals: {
    __proto__: null,
    SIGHUP: 1,
    SIGINT: 2,
    SIGQUIT: 3,
    SIGILL: 4,
    SIGTRAP: 5,
    SIGABRT: 6,
    SIGIOT: 6,
    SIGBUS: 7,
    SIGFPE: 8,
    SIGKILL: 9,
    SIGUSR1: 10,
    SIGSEGV: 11,
    SIGUSR2: 12,
    SIGPIPE: 13,
    SIGALRM: 14,
    SIGTERM: 15,
    SIGCHLD: 17,
    SIGSTKFLT: 16,
    SIGCONT: 18,
    SIGSTOP: 19,
    SIGTSTP: 20,
    SIGTTIN: 21,
    SIGTTOU: 22,
    SIGURG: 23,
    SIGXCPU: 24,
    SIGXFSZ: 25,
    SIGVTALRM: 26,
    SIGPROF: 27,
    SIGWINCH: 28,
    SIGIO: 29,
    SIGPOLL: 29,
    SIGPWR: 30,
    SIGSYS: 31,
  },
  priority: {
    __proto__: null,
    PRIORITY_LOW: 19,
    PRIORITY_BELOW_NORMAL: 10,
    PRIORITY_NORMAL: 0,
    PRIORITY_ABOVE_NORMAL: -7,
    PRIORITY_HIGH: -14,
    PRIORITY_HIGHEST: -20,
  },
};
Object.freeze(constants);
Object.freeze(constants.dlopen);
Object.freeze(constants.errno);
Object.freeze(constants.signals);
Object.freeze(constants.priority);

export default {
  EOL,
  devNull,
  constants,
  availableParallelism,
  arch,
  cpus,
  endianness,
  freemem,
  getPriority,
  homedir,
  hostname,
  loadavg,
  machine,
  networkInterfaces,
  platform,
  release,
  setPriority,
  tmpdir,
  totalmem,
  type,
  uptime,
  userInfo,
  version,
};
