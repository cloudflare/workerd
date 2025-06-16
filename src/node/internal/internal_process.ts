// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Our implementation of process.nextTick is just queueMicrotask. The timing
// of when the queue is drained is different, as is the error handling so this
// is only an approximation of process.nextTick semantics. Hopefully it's good
// enough because we really do not want to implement Node.js' idea of a nextTick
// queue.

import { validateObject } from 'node-internal:validators';

import {
  ERR_EPERM,
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_OUT_OF_RANGE,
  ERR_UNSUPPORTED_OPERATION,
  NodeError,
} from 'node-internal:internal_errors';

import {
  type EmitWarningOptions,
  default as processImpl,
} from 'node-internal:process';
import {
  _initialized as eventsInitialized,
  default as EventEmitter,
} from 'node-internal:events';
import type { Buffer } from 'node:buffer';
import { parseEnv } from 'node-internal:internal_utils';
import type * as NodeFS from 'node:fs';

declare global {
  const Cloudflare: {
    readonly compatibilityFlags: Record<string, boolean>;
  };
}

const { compatibilityFlags } = Cloudflare;

export const platform = compatibilityFlags.unsupported_process_actual_platform
  ? processImpl.platform
  : 'linux';

export let versions = processImpl.versions,
  version: string | undefined,
  title: string | undefined,
  argv: string[] | undefined,
  argv0: string | undefined,
  execArgv: string[] | undefined,
  arch: string | undefined,
  config: object | undefined,
  pid: number | undefined,
  ppid: number | undefined,
  allowedNodeEnvironmentFlags: Set<string> | undefined;

export let hrtime:
  | ({
      (time?: [number, number]): [number, number];
      bigint(): bigint;
    } & ((time?: [number, number]) => [number, number]))
  | undefined;

let _setSourceMapsEnabled:
  | ((
      _enabled: boolean,
      _options?: { nodeModules: boolean; generatedCode: boolean }
    ) => void)
  | undefined;
let _getSourceMapsSupport:
  | (() => {
      enabled: boolean;
      nodeModules: boolean;
      generatedCode: boolean;
    })
  | undefined;
let _emitWarning:
  | ((
      warning: string | Error,
      options?: ErrorConstructor | string | EmitWarningOptions,
      codeOrCtor?: ErrorConstructor | string,
      maybeCtor?: ErrorConstructor
    ) => void)
  | undefined;
let _uptime: (() => number) | undefined;
let _loadEnvFile: ((path: string | URL | Buffer) => void) | undefined;
let _getegid: (() => number) | undefined;
let _getgid: (() => number) | undefined;
let _getgroups: (() => number[]) | undefined;
let _geteuid: (() => number) | undefined;
let _getuid: (() => number) | undefined;
let _setegid: ((id: number | string) => void) | undefined;
let _setgid: ((id: number | string) => void) | undefined;
let _setgroups: ((groups: number[]) => void) | undefined;
let _seteuid: ((id: number | string) => void) | undefined;
let _setuid: ((id: number | string) => void) | undefined;
let _initgroups:
  | ((user: number | string, extractGroup: number | string) => void)
  | undefined;
let _abort: (() => void) | undefined;

export {
  _setSourceMapsEnabled as setSourceMapsEnabled,
  _getSourceMapsSupport as getSourceMapsSupport,
  _emitWarning as emitWarning,
  _uptime as uptime,
  _loadEnvFile as loadEnvFile,
  _getegid as getegid,
  _getgid as getgid,
  _getgroups as getgroups,
  _geteuid as geteuid,
  _getuid as getuid,
  _setegid as setegid,
  _setgid as setgid,
  _setgroups as setgroups,
  _seteuid as seteuid,
  _setuid as setuid,
  _initgroups as initgroups,
  _abort as abort,
};

// TODO(soon): Implement stdio along with TTY streams (and as a requirement for removing experimental).
export const stdin = undefined;
export const stdout = undefined;
export const stderr = undefined;

// TODO(soon): Implement along with FS work (and as a requirement for removing experimental).
export const chdir = undefined;
export const cwd = undefined;
export const umask = undefined;

if (compatibilityFlags.enable_nodejs_process_v2) {
  versions = processImpl.versions;

  version = `v${processImpl.versions.node}`;

  title = 'workerd';

  argv = ['workerd'];

  argv0 = 'workerd';

  execArgv = [];

  arch = 'x64';

  config = {
    target_defaults: {},
    variables: {},
  };

  // For simplicity and polyfill expectations, we assign pid 1 to the process and pid 0 to the parent
  pid = 1;
  ppid = 0;

  allowedNodeEnvironmentFlags = new Set();

  hrtime = Object.assign(
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

  _setSourceMapsEnabled = setSourceMapsEnabled;
  _getSourceMapsSupport = getSourceMapsSupport;
  _emitWarning = emitWarning as (
    warning: string | Error,
    options?: ErrorConstructor | string | EmitWarningOptions,
    codeOrCtor?: ErrorConstructor | string,
    maybeCtor?: ErrorConstructor
  ) => void;
  _uptime = uptime;
  _loadEnvFile = loadEnvFile;
  _getegid = getegid;
  _getgid = getgid;
  _getgroups = getgroups;
  _geteuid = geteuid;
  _getuid = getuid;
  _setegid = setegid;
  _setgid = setgid;
  _setgroups = setgroups;
  _seteuid = seteuid;
  _setuid = setuid;
  _initgroups = initgroups;
  _abort = abort;
}

function setSourceMapsEnabled(
  _enabled: boolean,
  _options?: { nodeModules: boolean; generatedCode: boolean }
): void {
  // no-op since we do not support disabling source maps
}

function getSourceMapsSupport(): {
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

// eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
export function nextTick(cb: Function, ...args: unknown[]): void {
  queueMicrotask(() => {
    // eslint-disable-next-line @typescript-eslint/no-unsafe-call
    cb(...args);
  });
}

interface ErrorWithDetail extends Error {
  detail?: unknown;
}

function emitWarning(warning: string | Error, ctor?: ErrorConstructor): void;
function emitWarning(
  warning: string | Error,
  type?: string,
  ctor?: ErrorConstructor
): void;
function emitWarning(
  warning: string | Error,
  type?: string,
  code?: string,
  ctor?: ErrorConstructor
): void;
function emitWarning(
  warning: string | Error,
  options?: ErrorConstructor | string | EmitWarningOptions,
  codeOrCtor?: ErrorConstructor | string,
  maybeCtor?: ErrorConstructor
): void {
  let err: Error;
  let name = 'Warning';
  let detail: string | undefined;
  let code: string | undefined;
  let ctor: ErrorConstructor | undefined;

  // Handle different overloads
  if (typeof options === 'object' && !Array.isArray(options)) {
    // emitWarning(warning, options)
    if (options.type) name = options.type;
    if (options.code) code = options.code;
    if (options.detail) detail = options.detail;
    ctor = options.ctor;
  } else if (typeof options === 'string') {
    // emitWarning(warning, type, ...)
    name = options;
    if (typeof codeOrCtor === 'string') {
      // emitWarning(warning, type, code, ctor)
      code = codeOrCtor;
      if (typeof maybeCtor === 'function') {
        ctor = maybeCtor;
      } else if ((maybeCtor as unknown) !== undefined) {
        throw new ERR_INVALID_ARG_TYPE('ctor', 'function', maybeCtor);
      }
    } else if (typeof codeOrCtor === 'function') {
      // emitWarning(warning, type, ctor)
      ctor = codeOrCtor;
    } else if ((codeOrCtor as unknown) !== undefined) {
      throw new ERR_INVALID_ARG_TYPE('ctor', 'function', codeOrCtor);
    }
  } else if (typeof options === 'function') {
    // emitWarning(warning, ctor)
    ctor = options;
  } else if (options !== undefined) {
    throw new ERR_INVALID_ARG_TYPE('options', 'object', options);
  }

  // Convert string warning to Error
  if (typeof warning === 'string') {
    // Use the provided constructor if available, otherwise use Error
    const ErrorConstructor = ctor || Error;
    err = new ErrorConstructor(warning);
    err.name = name;
  } else if (warning instanceof Error) {
    err = warning;
    // Override name if provided
    if (name && name !== 'Warning') {
      err.name = name;
    }
  } else {
    throw new ERR_INVALID_ARG_TYPE('warning', 'string or Error', warning);
  }

  // Add code if provided
  if (code) {
    (err as NodeError).code = code;
  }

  // Add detail if provided
  if (detail && typeof detail === 'string') {
    (err as ErrorWithDetail).detail = detail;
  }

  // Capture stack trace using the provided constructor or emitWarning itself
  // This excludes the constructor (and frames above it) from the stack trace
  Error.captureStackTrace(err, ctor || emitWarning);

  // Emit the warning event on the process object
  // Use nextTick to ensure the warning is emitted asynchronously
  queueMicrotask(() => {
    process.emit('warning', err);
  });
}

// Decide if a value can round-trip to JSON without losing any information.
function isJsonSerializable(
  value: unknown,
  seen: Set<unknown> = new Set()
): boolean {
  switch (typeof value) {
    case 'boolean':
    case 'number':
    case 'string':
      return true;

    case 'object': {
      if (value === null) {
        return true;
      }

      if (seen.has(value)) {
        // Don't allow cycles or aliases. (Non-cyclic aliases technically could be OK, but a
        // round trip to JSON would lose the fact that they are aliases.)
        return false;
      }
      seen.add(value);

      // TODO(revisit): While any object that implements the toJSON function is
      // generally expected to be JSON serializable, when working with jsrpc
      // targets, the `toJSON` property ends up being Proxied and appears to be
      // a legit property on some object types when it really shouldn't be, causing
      // issues with certain types of bindings. Fun!
      // Commenting this out instead of removing it because it would be great if
      // we could find a way to support this reliably.
      //
      // if (typeof value.toJSON === 'function') {
      //   // This type is explicitly designed to be JSON-serialized so we'll accept it.
      //   return true;
      // }

      // We only consider objects to be serializable if they are plain objects or plain arrays.
      // Technically, JSON can serialize any subclass of Object (as well as objects with null
      // prototypes), but the round trip would lose information about the original type. Hence,
      // we assume that any env var containing such things is not intended to be appear as JSON
      // in process.env. For example, we wouldn't want a KV namespace to show up in process.env as
      // "{}" -- this would be weird.
      switch (Object.getPrototypeOf(value)) {
        case Object.prototype:
          // Note that Object.values() only returns string-keyed values, not symbol-keyed.
          return Object.values(value).every((prop) =>
            isJsonSerializable(prop, seen)
          );
        case Array.prototype:
          return (value as Array<unknown>).every((elem: unknown) =>
            isJsonSerializable(elem, seen)
          );
        default:
          return false;
      }
    }

    default:
      return false;
  }
}

function getInitialEnv(): Record<string, string> {
  const env: Record<string, string> = {};
  for (const [key, value] of Object.entries(processImpl.getEnvObject())) {
    // Workers environment variables can have a variety of types, but process.env vars are
    // strictly strings. We want to convert our workers env into process.env, but allowing
    // process.env to contain non-strings would probably break Node apps.
    //
    // As a compromise, we say:
    // - Workers env vars that are plain strings are unchanged in process.env.
    // - Workers env vars that can be represented as JSON will be JSON-stringified in process.env.
    // - Anything else will be omitted.
    //
    // Note that you might argue that, at the config layer, it's possible to differentiate between
    // plain strings and JSON values that evaluated to strings. Wouldn't it be nice if we could
    // check which way the binding was originally configured in order to decide whether to
    // represent it plain or as JSON here. However, there is no way to tell just by looking at
    // the `env` object inside a Worker whether a particular var was originally configured as
    // plain text, or as JSON that evaluated to a string. Either way, you just get a string. And
    // indeed, the Workers Runtime itself does not necessarily know this. In many cases it does
    // know, but in general the abstraction the Runtime intends to provide is that `env` is just
    // a JavaScript object, and how exactly the contents were originally represented is not
    // intended to be conveyed. This is important because, for example, we could extend dynamic
    // dispatch bindings in the future such that the caller can specify `env` directly, and in
    // that case the caller would simply specify a JS object, without JSON or any other
    // serialization involved. In this case, there would be no way to know if a string var was
    // "supposed to be" raw text vs. JSON.
    //
    // So, we have to do the best we can given just what we know -- the JavaScript object that is
    // `env`.
    //
    // As a consolation, this is consistent with how variables are defined in wrangler.toml: you
    // do not explicitly specify whether a variable is text or JSON. If you define a variable with
    // a simple string value, it gets configured as a text var. If you specify an object, then it's
    // configured as JSON.

    if (typeof value === 'string') {
      env[key] = value;
    } else if (isJsonSerializable(value)) {
      env[key] = JSON.stringify(value);
    }
  }
  return env;
}

export const env = new Proxy(getInitialEnv(), {
  // Per Node.js rules. process.env values must be coerced to strings.
  // When defined using defineProperty, the property descriptor must be writable,
  // configurable, and enumerable using just a falsy check. Getters and setters
  // are not permitted.
  set(obj: object, prop: PropertyKey, value: unknown): boolean {
    if (typeof prop === 'symbol' || typeof value === 'symbol')
      throw new TypeError(`Cannot convert a symbol value to a string`);
    return Reflect.set(obj, prop, `${value}`);
  },
  defineProperty(
    obj: object,
    prop: PropertyKey,
    descriptor: PropertyDescriptor
  ): boolean {
    validateObject(descriptor, 'descriptor');
    if (Reflect.has(descriptor, 'get') || Reflect.has(descriptor, 'set')) {
      throw new ERR_INVALID_ARG_VALUE(
        'descriptor',
        descriptor,
        'process.env value must not have getter/setter'
      );
    }
    if (!descriptor.configurable) {
      throw new ERR_INVALID_ARG_VALUE(
        'descriptor.configurable',
        descriptor,
        'process.env value must be configurable'
      );
    }
    if (!descriptor.enumerable) {
      throw new ERR_INVALID_ARG_VALUE(
        'descriptor.enumerable',
        descriptor,
        'process.env value must be enumerable'
      );
    }
    if (!descriptor.writable) {
      throw new ERR_INVALID_ARG_VALUE(
        'descriptor.writable',
        descriptor,
        'process.env value must be writable'
      );
    }
    if (Reflect.has(descriptor, 'value')) {
      if (typeof prop === 'symbol' || typeof descriptor.value === 'symbol')
        throw new TypeError(`Cannot convert a symbol value to a string`);
      Reflect.set(descriptor, 'value', `${descriptor.value}`);
    } else {
      throw new ERR_INVALID_ARG_VALUE(
        'descriptor.value',
        descriptor,
        'process.env value must be specified explicitly'
      );
    }
    if (typeof prop === 'symbol')
      throw new TypeError(`Cannot convert a symbol value to a string`);
    return Reflect.defineProperty(obj, prop, descriptor);
  },
});

export function getBuiltinModule(id: string): object {
  return processImpl.getBuiltinModule(id);
}

export function exit(code: number): void {
  processImpl.exitImpl(code);
}

function abort(): void {
  processImpl.exitImpl(1);
}

// In future we may return accurate uptime information here, but returning
// zero at least allows code paths to work correctly in most cases, and is
// not a completely unreasonable interpretation of Cloudflare's execution
// model assumptions.
function uptime(): number {
  return 0;
}

// TODO(soon): Support with proper process.cwd() resolution along with
//             test in process-nodejs-test.
function loadEnvFile(path: string | URL | Buffer = '/bundle/.env'): void {
  if (!compatibilityFlags.experimental || !compatibilityFlags.nodejs_compat) {
    throw new ERR_UNSUPPORTED_OPERATION();
  }
  const { readFileSync } = process.getBuiltinModule('fs') as typeof NodeFS;
  const parsed = parseEnv(
    readFileSync(path instanceof URL ? path : path.toString(), 'utf8')
  );
  Object.assign(process.env, parsed);
}

// On the virtual filesystem, we only support group 0
function getegid(): number {
  return 0;
}

function getgid(): number {
  return 0;
}

function getgroups(): number[] {
  return [0];
}

// On the virtual filesystem, we only support user 0
function geteuid(): number {
  return 0;
}

function getuid(): number {
  return 0;
}

// We support group 0 but no others, with the virtual filesystem.
function setegid(id: number | string): void {
  if (id !== 0) throw new ERR_EPERM({ syscall: 'setegid' });
}

// We support group 0 but no others, with the virtual filesystem.
function setgid(id: number | string): void {
  if (id !== 0) throw new ERR_EPERM({ syscall: 'setgid' });
}

// We support group 0 but no others, with the virtual filesystem.
function seteuid(id: number | string): void {
  if (id !== 0) throw new ERR_EPERM({ syscall: 'seteuid' });
}

// We support group 0 but no others, with the virtual filesystem.
function setuid(id: number | string): void {
  if (id !== 0) throw new ERR_EPERM({ syscall: 'setuid' });
}

// We don't support setting or initializing arbitrary groups on the virtual filesystem.
function setgroups(_groups: number[]): void {
  throw new ERR_EPERM({ syscall: 'setgroups' });
}

function initgroups(
  _user: number | string,
  _extractGroup: number | string
): void {
  throw new ERR_EPERM({ syscall: 'initgroups' });
}

// The following features does not include deprecated or experimental flags mentioned in
// https://nodejs.org/docs/latest/api/process.html
export const features = Object.freeze({
  // A boolean value that is true if the current Node.js build is caching builtin modules.
  cached_builtins: true,
  // A boolean value that is true if the current Node.js build is a debug build.
  debug: false,
  // A boolean value that is true if the current Node.js build includes the inspector.
  inspector: false,
  // A boolean value that is true if the current Node.js build supports loading ECMAScript modules using require().
  // TODO(soon): Update this when we support ESM modules through require().
  require_module: false,
  // A boolean value that is true if the current Node.js build includes support for TLS.
  tls: true,
});

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
  getegid: typeof _getegid;
  getgid: typeof _getgid;
  getgroups: typeof _getgroups;
  geteuid: typeof _geteuid;
  getuid: typeof _getuid;
  setegid: typeof _setegid;
  setgid: typeof _setgid;
  setgroups: typeof _setgroups;
  seteuid: typeof _seteuid;
  setuid: typeof _setuid;
  initgroups: typeof _initgroups;
  setSourceMapsEnabled: typeof _setSourceMapsEnabled;
  getSourceMapsSupport: typeof _getSourceMapsSupport;
  nextTick: typeof nextTick;
  emitWarning: typeof _emitWarning;
  env: typeof env;
  exit: typeof exit;
  abort: typeof _abort;
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
  uptime: typeof _uptime;
  loadEnvFile: typeof _loadEnvFile;
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

// NOTE: all properties added here must also be added on the process.ts re-exports
const process = {
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
  getegid: _getegid,
  getgid: _getgid,
  getgroups: _getgroups,
  geteuid: _geteuid,
  getuid: _getuid,
  setegid: _setegid,
  setgid: _setgid,
  setgroups: _setgroups,
  seteuid: _seteuid,
  setuid: _setuid,
  initgroups: _initgroups,
  setSourceMapsEnabled: _setSourceMapsEnabled,
  getSourceMapsSupport: _getSourceMapsSupport,
  nextTick,
  emitWarning: _emitWarning,
  env,
  exit,
  abort: _abort,
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
  uptime: _uptime,
  loadEnvFile: _loadEnvFile,
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
} as Process;

export default process;

// Must be a var binding to support TDZ checks
// eslint-disable-next-line
export var _initialized = true;

// Private export, not to be reexported
export function _initProcess(): void {
  if (!compatibilityFlags.enable_nodejs_process_v2) return;
  Object.setPrototypeOf(process, EventEmitter.prototype as object);
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
}

// If process executes first, events will call _initProcess
// If events executes first, process calls _initProcess itself
// This allows us to handle the cycle between process and Events
if (eventsInitialized) {
  _initProcess();
}
