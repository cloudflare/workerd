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
  ERR_METHOD_NOT_IMPLEMENTED,
} from 'node-internal:internal_errors';
import processImpl from 'node-internal:process';
import { Buffer } from 'node-internal:internal_buffer';
import { parseEnv } from 'node-internal:internal_utils';
import { Writable } from 'node-internal:streams_writable';
import { writeSync } from 'node-internal:internal_fs_sync';
import { ReadStream } from 'node-internal:internal_fs_streams';
import {
  platform,
  nextTick,
  emitWarning,
  env,
  features,
  _setEventsProcess,
} from 'node-internal:internal_process';
import { validateString } from 'node-internal:validators';
import internalAssert from 'node-internal:internal_assert';
import type { Readable } from 'node-internal:streams_readable';
import type * as NodeFS from 'node:fs';

export { platform, nextTick, emitWarning, env, features };

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

export function ref(): void {
  // no-op
}

export function unref(): void {
  // no-op
}

export function setUncaughtExceptionCaptureCallback(
  _fn?: (...args: unknown[]) => void
): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED(
    'process.setUncaughtExceptionCaptureCallback'
  );
}

export function hasUncaughtExceptionCaptureCallback(): boolean {
  throw new ERR_METHOD_NOT_IMPLEMENTED(
    'process.hasUncaughtExceptionCaptureCallback'
  );
}

export function kill(_pid: number, _signal?: string | number): boolean {
  throw new ERR_METHOD_NOT_IMPLEMENTED('process.kill');
}

export function binding(_name: string): unknown {
  throw new ERR_METHOD_NOT_IMPLEMENTED('process.binding');
}

export function dlopen(
  _module: unknown,
  _filename: string,
  _flags?: number
): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('process.dlopen');
}

export const exitCode: number | undefined = undefined;

export function getActiveResourcesInfo(): string[] {
  throw new ERR_METHOD_NOT_IMPLEMENTED('process.getActiveResourcesInfo');
}

export function memoryUsage(): {
  rss: number;
  heapTotal: number;
  heapUsed: number;
  external: number;
  arrayBuffers: number;
} {
  return {
    rss: 0,
    heapTotal: 0,
    heapUsed: 0,
    external: 0,
    arrayBuffers: 0,
  };
}

export function resourceUsage(): {
  userCPUTime: number;
  systemCPUTime: number;
  maxRSS: number;
  sharedMemorySize: number;
  unsharedDataSize: number;
  unsharedStackSize: number;
  minorPageFault: number;
  majorPageFault: number;
  swappedOut: number;
  fsRead: number;
  fsWrite: number;
  ipcSent: number;
  ipcReceived: number;
  signalsCount: number;
  voluntaryContextSwitches: number;
  involuntaryContextSwitches: number;
} {
  return {
    userCPUTime: 0,
    systemCPUTime: 0,
    maxRSS: 0,
    sharedMemorySize: 0,
    unsharedDataSize: 0,
    unsharedStackSize: 0,
    minorPageFault: 0,
    majorPageFault: 0,
    swappedOut: 0,
    fsRead: 0,
    fsWrite: 0,
    ipcSent: 0,
    ipcReceived: 0,
    signalsCount: 0,
    voluntaryContextSwitches: 0,
    involuntaryContextSwitches: 0,
  };
}

export function threadCpuUsage(): {
  user: number;
  system: number;
} {
  throw new ERR_METHOD_NOT_IMPLEMENTED('process.threadCpuUsage');
}

export function cpuUsage(_previousValue?: { user: number; system: number }): {
  user: number;
  system: number;
} {
  throw new ERR_METHOD_NOT_IMPLEMENTED('process.cpuUsage');
}

// Properties and constants
export const channel = null;
export const connected = false;
export const debugPort = 0; // may be implemented to align with inspector in future
export const noDeprecation = false;
export const traceDeprecation = false;
export const throwDeprecation = false;
export const sourceMapsEnabled = false;
export const execPath = '';

export const permission = {
  has: (): boolean => {
    throw new ERR_METHOD_NOT_IMPLEMENTED('process.permission.has');
  },
};

export const release = {
  name: 'node',
  lts: true,
  sourceUrl: '',
  headersUrl: '',
};

export const report = {
  compact: false,
  directory: '',
  filename: '',
  getReport: (): Record<string, unknown> => {
    throw new ERR_METHOD_NOT_IMPLEMENTED('process.report.getReport');
  },
  reportOnFatalError: false,
  reportOnSignal: false,
  reportOnUncaughtException: false,
  signal: 'SIGUSR2',
  writeReport: (): string => {
    throw new ERR_METHOD_NOT_IMPLEMENTED('process.report.writeReport');
  },
};

export const finalization = {
  register: (): void => {
    throw new ERR_METHOD_NOT_IMPLEMENTED('process.finalization.register');
  },
  registerBeforeExit: (): void => {
    throw new ERR_METHOD_NOT_IMPLEMENTED(
      'process.finalization.registerBeforeExit'
    );
  },
  unregister: (): void => {
    throw new ERR_METHOD_NOT_IMPLEMENTED('process.finalization.unregister');
  },
};

// Additional undocumented APIs
export function _rawDebug(...args: unknown[]): void {
  console.log(...args);
}

export const moduleLoadList: string[] = [];
export const _preload_modules: string[] = [];

export function _linkedBinding(_name: string): unknown {
  throw new ERR_METHOD_NOT_IMPLEMENTED('process._linkedBinding');
}

export const domain = null;
export const _exiting = false;

export function _getActiveRequests(): unknown[] {
  return [];
}

export function _getActiveHandles(): unknown[] {
  return [];
}

export function reallyExit(code?: number): void {
  processImpl.exitImpl(code || 0);
}

export function _kill(_pid: number, _signal: number): boolean {
  throw new ERR_METHOD_NOT_IMPLEMENTED('process._kill');
}

export function constrainedMemory(): number {
  return 0;
}

export function availableMemory(): number {
  // This may be implemented in future, for now this matches unenv
  return 0;
}

export function execve(
  _file: string,
  _args: string[],
  _env: Record<string, string>
): never {
  throw new ERR_METHOD_NOT_IMPLEMENTED('process.execve');
}

export function openStdin(): typeof stdin {
  return stdin;
}

export function _fatalException(_err: Error): boolean {
  return false;
}

export function _tickCallback(): void {
  // no-op
}

export function _debugProcess(_pid: number): void {
  // no-op
}

export function _debugEnd(): void {
  // no-op
}

export function _startProfilerIdleNotifier(): void {
  // no-op
}

export function _stopProfilerIdleNotifier(): void {
  // no-op
}

export function send(_message: unknown, _sendHandle?: unknown): boolean {
  return false;
}

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
  exitCode: typeof exitCode;
  channel: typeof channel;
  connected: typeof connected;
  binding: typeof binding;
  debugPort: typeof debugPort;
  dlopen: typeof dlopen;
  finalization: typeof finalization;
  getActiveResourcesInfo: typeof getActiveResourcesInfo;
  setUncaughtExceptionCaptureCallback: typeof setUncaughtExceptionCaptureCallback;
  hasUncaughtExceptionCaptureCallback: typeof hasUncaughtExceptionCaptureCallback;
  memoryUsage: typeof memoryUsage;
  noDeprecation: typeof noDeprecation;
  permission: typeof permission;
  release: typeof release;
  report: typeof report;
  resourceUsage: typeof resourceUsage;
  send: typeof send;
  traceDeprecation: typeof traceDeprecation;
  throwDeprecation: typeof throwDeprecation;
  sourceMapsEnabled: typeof sourceMapsEnabled;
  stdin: typeof stdin;
  stdout: typeof stdout;
  stderr: typeof stderr;
  threadCpuUsage: typeof threadCpuUsage;
  cpuUsage: typeof cpuUsage;
  execPath: typeof execPath;
  constrainedMemory: typeof constrainedMemory;
  availableMemory: typeof availableMemory;
  execve: typeof execve;
  openStdin: typeof openStdin;
  _rawDebug: typeof _rawDebug;
  moduleLoadList: typeof moduleLoadList;
  _linkedBinding: typeof _linkedBinding;
  domain: typeof domain;
  _exiting: typeof _exiting;
  _getActiveRequests: typeof _getActiveRequests;
  _getActiveHandles: typeof _getActiveHandles;
  reallyExit: typeof reallyExit;
  _kill: typeof _kill;
  _fatalException: typeof _fatalException;
  _tickCallback: typeof _tickCallback;
  _debugProcess: typeof _debugProcess;
  _debugEnd: typeof _debugEnd;
  _startProfilerIdleNotifier: typeof _startProfilerIdleNotifier;
  _stopProfilerIdleNotifier: typeof _stopProfilerIdleNotifier;
  _preload_modules: typeof _preload_modules;
}

export const _disconnect = undefined,
  _handleQueue = undefined,
  _pendingMessage = undefined,
  _channel = undefined,
  _send = undefined;

export let assert: typeof internalAssert.ok | undefined = undefined;

if (!Cloudflare.compatibilityFlags.remove_nodejs_compat_eol_v23) {
  assert = internalAssert.ok.bind(internalAssert);
}

const _process = {
  assert,
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
  cpuUsage,
  execPath,
  constrainedMemory,
  availableMemory,
  execve,
  openStdin,
  _rawDebug,
  moduleLoadList,
  _linkedBinding,
  domain,
  _exiting,
  _getActiveRequests,
  _getActiveHandles,
  reallyExit,
  _kill,
  _fatalException,
  _tickCallback,
  _debugProcess,
  _debugEnd,
  _startProfilerIdleNotifier,
  _stopProfilerIdleNotifier,
  _preload_modules,
  _disconnect,
  _handleQueue,
  _pendingMessage,
  _channel,
  _send,
};

const process: Process = Object.setPrototypeOf(
  _process,
  EventEmitter.prototype as object
) as Process;
EventEmitter.call(process);

Object.defineProperty(process, Symbol.toStringTag, {
  value: 'process',
  configurable: true,
});

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

export const _events = process._events,
  _eventsCount = process._eventsCount,
  _maxListeners = process._maxListeners;

export default process;
