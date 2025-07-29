// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

// Process is implemented under node-internal: so that we can have node:process
// resolve to node-internal:public_process OR node-internal:legacy_process depending
// on whether the enable_nodejs_process_v2 compat flag is set.

import { default as EventEmitter } from 'node-internal:events';
import {
  ERR_EPERM,
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_OUT_OF_RANGE,
  ERR_UNSUPPORTED_OPERATION,
} from 'node-internal:internal_errors';
import processImpl from 'node-internal:process';
import { Buffer } from 'node-internal:internal_buffer';
import { parseEnv } from 'node-internal:internal_utils';
import { Writable } from 'node-internal:streams_writable';
import { writeSync } from 'node-internal:internal_fs_sync';
import { ReadStream } from 'node-internal:internal_fs_streams';
import type * as NodeFS from 'node:fs';
import {
  platform,
  nextTick,
  emitWarning,
  env,
  features,
  _setEventsProcess,
} from 'node-internal:internal_process';
import { validateString } from 'node-internal:validators';
import type { Readable } from 'node-internal:streams_readable';

export { platform, nextTick, emitWarning, env, features };

const workerdExperimental = !!Cloudflare.compatibilityFlags['experimental'];
const nodeJsCompat = !!Cloudflare.compatibilityFlags['nodejs_compat'];

// For stdin, we emulate `node foo.js < /dev/null`
export const stdin = new ReadStream(null, {
  fd: 0,
  autoClose: false,
}) as Readable & {
  fd: number;
};
stdin.fd = 0;

function chunkToBuffer(
  chunk: Buffer | ArrayBufferView | DataView | string,
  encoding: BufferEncoding
): Buffer {
  if (Buffer.isBuffer(chunk)) {
    return chunk;
  }
  if (typeof chunk === 'string') {
    return Buffer.from(chunk, encoding);
  }
  return Buffer.from(chunk.buffer, chunk.byteOffset, chunk.byteLength);
}

// For stdout, we emulate `nohup node foo.js`
class SyncWriteStream extends Writable {
  fd: number;
  readable: boolean;
  _type = 'fs';
  _isStdio = true;
  constructor(fd: number) {
    super({ autoDestroy: true });
    this.fd = fd;
    this.readable = false;
  }
  override _write(
    chunk: string | Buffer | ArrayBufferView | DataView,
    encoding: BufferEncoding,
    cb: (error?: Error | null) => void
  ): void {
    try {
      writeSync(this.fd, chunkToBuffer(chunk, encoding));
    } catch (e: unknown) {
      cb(e as Error);
      return;
    }
    cb();
  }
}

export const stdout = new SyncWriteStream(1);
export const stderr = new SyncWriteStream(2);

export function chdir(path: string | Buffer | URL): void {
  validateString(path, 'directory');
  processImpl.setCwd(path);
}

export const cwd = processImpl.getCwd.bind(processImpl);

// We do not support setting the umask as we do not have a fine-grained
// permissions on the VFS. Instead we only support validation of input
// and then ignoring it.
export function umask(mask: number | string | undefined): number {
  if (mask !== undefined) {
    if (typeof mask === 'string') {
      if (!/^[0-7]+$/.test(mask)) {
        throw new ERR_INVALID_ARG_VALUE(
          'mask',
          mask,
          'must be a 32-bit unsigned integer or an octal string'
        );
      }
      mask = parseInt(mask, 8);
    } else if (typeof mask !== 'number') {
      throw new ERR_INVALID_ARG_TYPE('mask', 'number', mask);
    }
    if (mask < 0 || mask > 0xffffffff || !Number.isInteger(mask)) {
      throw new ERR_INVALID_ARG_VALUE(
        'mask',
        mask,
        'must be a 32-bit unsigned integer or an octal string'
      );
    }
  }
  // just return Node.js default of 18
  return 18;
}

export const versions = processImpl.versions;

export const version = `v${processImpl.versions.node}`;

export const title = 'workerd';

export const argv = ['workerd'];

export const argv0 = 'workerd';

export const execArgv = [];

export const arch = 'x64';

export const config = {
  target_defaults: {},
  variables: {},
};

// For simplicity and polyfill expectations, we assign pid 1 to the process and pid 0 to the "parent".
export const pid = 1;
export const ppid = 0;

export const allowedNodeEnvironmentFlags = new Set();

export function setSourceMapsEnabled(
  _enabled: boolean,
  _options?: { nodeModules: boolean; generatedCode: boolean }
): void {
  // no-op since we do not support disabling source maps
}

export function getSourceMapsSupport(): {
  enabled: boolean;
  nodeModules: boolean;
  generatedCode: boolean;
} {
  return {
    enabled: false,
    nodeModules: false,
    generatedCode: false,
  };
}

export const hrtime = Object.assign(
  function hrtime(time?: [number, number]): [number, number] {
    if (time !== undefined) {
      if (!Array.isArray(time))
        throw new ERR_INVALID_ARG_TYPE('time', 'tuple of integers', time);
      else if ((time as number[]).length !== 2)
        throw new ERR_OUT_OF_RANGE('time', '2', time);
    }
    const nanoTime = BigInt(Date.now()) * 1_000_000n;
    const nanosBigint = nanoTime % 1_000_000_000n;
    let seconds = Number((nanoTime - nanosBigint) / 1_000_000_000n);
    let nanos = Number(nanosBigint);
    if (time) {
      const [prevSeconds, prevNanos] = time;
      seconds -= prevSeconds;
      nanos -= prevNanos;
      // We don't have nanosecond-level precision, but if we
      // did (say with the introduction of randomness), this
      // is how we would handle nanosecond underflow
      if (nanos < 0) {
        seconds -= 1;
        nanos += 1e9;
      }
    }
    return [seconds, nanos];
  },
  {
    bigint: function (): bigint {
      return BigInt(Date.now()) * 1_000_000n;
    },
  }
);

// Unsupported features - implemented as 'undefined' so they can still be imported
// statically via `import { channel } from 'node:process'` without breaking.
// In future, these may yet be be possibly implemented or stubbed.
export const exitCode = undefined,
  channel = undefined,
  connected = undefined,
  binding = undefined,
  debugPort = undefined,
  dlopen = undefined,
  finalization = undefined,
  getActiveResourcesInfo = undefined,
  setUncaughtExceptionCaptureCallback = undefined,
  hasUncaughtExceptionCaptureCallback = undefined,
  memoryUsage = undefined,
  noDeprecation = undefined,
  permission = undefined,
  release = undefined,
  report = undefined,
  resourceUsage = undefined,
  send = undefined,
  traceDeprecation = undefined,
  throwDeprecation = undefined,
  sourceMapsEnabled = undefined,
  threadCpuUsage = undefined,
  kill = undefined,
  ref = undefined,
  unref = undefined;

export function getBuiltinModule(id: string): object {
  return processImpl.getBuiltinModule(id);
}

export function exit(code: number): void {
  processImpl.exitImpl(code);
}

export function abort(): void {
  exit(1);
}

// In future we may return accurate uptime information here, but returning
// zero at least allows code paths to work correctly in most cases, and is
// not a completely unreasonable interpretation of Cloudflare's execution
// model assumptions.
export function uptime(): number {
  return 0;
}

export function loadEnvFile(path: string | URL | Buffer = '.env'): void {
  if (!workerdExperimental || !nodeJsCompat) {
    throw new ERR_UNSUPPORTED_OPERATION();
  }
  const { readFileSync } = process.getBuiltinModule('fs') as typeof NodeFS;
  const parsed = parseEnv(
    readFileSync(path instanceof URL ? path : path.toString(), 'utf8')
  );
  Object.assign(process.env, parsed);
}

// On the virtual filesystem, we only support group 0
export function getegid(): number {
  return 0;
}

export function getgid(): number {
  return 0;
}

export function getgroups(): number[] {
  return [0];
}

// On the virtual filesystem, we only support user 0
export function geteuid(): number {
  return 0;
}

export function getuid(): number {
  return 0;
}

// We support group 0 but no others, with the virtual filesystem.
export function setegid(id: number | string): void {
  if (id !== 0) throw new ERR_EPERM({ syscall: 'setegid' });
}

// We support group 0 but no others, with the virtual filesystem.
export function setgid(id: number | string): void {
  if (id !== 0) throw new ERR_EPERM({ syscall: 'setgid' });
}

// We support group 0 but no others, with the virtual filesystem.
export function seteuid(id: number | string): void {
  if (id !== 0) throw new ERR_EPERM({ syscall: 'seteuid' });
}

// We support group 0 but no others, with the virtual filesystem.
export function setuid(id: number | string): void {
  if (id !== 0) throw new ERR_EPERM({ syscall: 'setuid' });
}

// We don't support setting or initializing arbitrary groups on the virtual filesystem.
export function setgroups(_groups: number[]): void {
  throw new ERR_EPERM({ syscall: 'setgroups' });
}

export function initgroups(
  _user: number | string,
  _extractGroup: number | string
): void {
  throw new ERR_EPERM({ syscall: 'initgroups' });
}

interface Process extends EventEmitter {
  version: typeof version;
  versions: typeof versions;
  title: typeof title;
  argv: typeof argv;
  argv0: typeof argv0;
  execArgv: typeof execArgv;
  arch: typeof arch;
  platform: typeof platform;
  config: typeof config;
  pid: typeof pid;
  ppid: typeof ppid;
  getegid: typeof getegid;
  getgid: typeof getgid;
  getgroups: typeof getgroups;
  geteuid: typeof geteuid;
  getuid: typeof getuid;
  setegid: typeof setegid;
  setgid: typeof setgid;
  setgroups: typeof setgroups;
  seteuid: typeof seteuid;
  setuid: typeof setuid;
  initgroups: typeof initgroups;
  setSourceMapsEnabled: typeof setSourceMapsEnabled;
  getSourceMapsSupport: typeof getSourceMapsSupport;
  nextTick: typeof nextTick;
  emitWarning: typeof emitWarning;
  env: typeof env;
  exit: typeof exit;
  abort: typeof abort;
  getBuiltinModule: typeof getBuiltinModule;
  features: typeof features;
  allowedNodeEnvironmentFlags: typeof allowedNodeEnvironmentFlags;
  kill: typeof kill;
  ref: typeof ref;
  unref: typeof unref;
  chdir: typeof chdir;
  cwd: typeof cwd;
  umask: typeof umask;
  hrtime: typeof hrtime;
  uptime: typeof uptime;
  loadEnvFile: typeof loadEnvFile;
  exitCode: undefined;
  channel: undefined;
  connected: undefined;
  binding: undefined;
  debugPort: undefined;
  dlopen: undefined;
  finalization: undefined;
  getActiveResourcesInfo: undefined;
  setUncaughtExceptionCaptureCallback: undefined;
  hasUncaughtExceptionCaptureCallback: undefined;
  memoryUsage: undefined;
  noDeprecation: undefined;
  permission: undefined;
  release: undefined;
  report: undefined;
  resourceUsage: undefined;
  send: undefined;
  traceDeprecation: undefined;
  throwDeprecation: undefined;
  sourceMapsEnabled: undefined;
  stdin: undefined;
  stdout: undefined;
  stderr: undefined;
  threadCpuUsage: undefined;
}

const _process = {
  version,
  versions,
  title,
  argv,
  argv0,
  execArgv,
  arch,
  platform,
  config,
  pid,
  ppid,
  getegid,
  getgid,
  getgroups,
  geteuid,
  getuid,
  setegid,
  setgid,
  setgroups,
  seteuid,
  setuid,
  initgroups,
  setSourceMapsEnabled,
  getSourceMapsSupport,
  nextTick,
  emitWarning,
  env,
  exit,
  abort,
  getBuiltinModule,
  features,
  allowedNodeEnvironmentFlags,
  kill,
  ref,
  unref,
  chdir,
  cwd,
  umask,
  hrtime,
  uptime,
  loadEnvFile,
  exitCode,
  channel,
  connected,
  binding,
  debugPort,
  dlopen,
  finalization,
  getActiveResourcesInfo,
  setUncaughtExceptionCaptureCallback,
  hasUncaughtExceptionCaptureCallback,
  memoryUsage,
  noDeprecation,
  permission,
  release,
  report,
  resourceUsage,
  send,
  traceDeprecation,
  throwDeprecation,
  sourceMapsEnabled,
  stdin,
  stdout,
  stderr,
  threadCpuUsage,
};

const process: Process = Object.setPrototypeOf(
  _process,
  EventEmitter.prototype as object
) as Process;
EventEmitter.call(process);

// We lazily attach unhandled rejection and rejection handled listeners
// to ensure performance optimizations remain in the no listener case
let addedUnhandledRejection = false,
  addedRejectionHandled = false;
process.on('newListener', (name) => {
  if (name === 'unhandledRejection' && !addedUnhandledRejection) {
    addEventListener(
      'unhandledrejection',
      function (evt: Event & { reason: unknown; promise: Promise<unknown> }) {
        process.emit('unhandledRejection', evt.reason, evt.promise);
      }
    );
    addedUnhandledRejection = true;
  }
  if (name === 'rejectionHandled' && !addedRejectionHandled) {
    addEventListener(
      'rejectionhandled',
      function (evt: Event & { reason: unknown; promise: Promise<unknown> }) {
        process.emit('rejectionHandled', evt.promise);
      }
    );
    addedRejectionHandled = true;
  }
});

_setEventsProcess(process);

export default process;
