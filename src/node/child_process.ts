// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import type {
  SpawnSyncReturns,
  SpawnSyncOptions,
  ChildProcess as _ChildProcess,
  ExecOptions,
  ExecException,
  ExecFileOptions,
  ExecSyncOptions,
  ForkOptions,
} from 'node:child_process';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import { EventEmitter } from 'node-internal:events';
import {
  validateFunction,
  validateObject,
  validateString,
  validateArray,
} from 'node-internal:validators';
import type { Readable, Writable } from 'stream';

export class ChildProcess extends EventEmitter implements _ChildProcess {
  stdin: Writable | null = null;
  stdout: Readable | null = null;
  stderr: Readable | null = null;
  killed: boolean = false;
  stdio: [
    Writable | null,
    Readable | null,
    Readable | null,
    Writable | Readable | null | undefined,
    Writable | Readable | null | undefined,
  ] = [null, null, null, null, null];
  connected: boolean = false;
  exitCode: number | null = null;
  signalCode: NodeJS.Signals | null = null;
  spawnargs: string[] = [];
  spawnfile: string;

  constructor() {
    super();
    throw new ERR_METHOD_NOT_IMPLEMENTED('child_process.ChildProcess');
  }

  kill(_signal?: NodeJS.Signals | number): boolean {
    return false;
  }

  send(
    _message: unknown,
    _sendHandle?: unknown,
    _options?: unknown,
    _callback?: unknown
  ): boolean {
    return false;
  }

  disconnect(): void {
    // Do nothing.
  }

  unref(): void {
    // Do nothing
  }

  ref(): void {
    // Do nothing
  }

  [Symbol.dispose](): void {
    this.kill();
  }
}

export function _forkChild(_fd: number, _serializationMode: number): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('child_process._forkChild');
}

export function exec(
  command: string,
  options: ExecOptions | undefined | null,
  callback?: (
    error: ExecException | null,
    stdout: string | Buffer,
    stderr: string | Buffer
  ) => void
): ChildProcess {
  validateString(command, 'command');
  if (options != null) {
    validateObject(options, 'options');
  }
  if (callback != null) {
    validateFunction(callback, 'callback');
  }
  throw new ERR_METHOD_NOT_IMPLEMENTED('child_process.exec');
}

export function execFile(
  _file: string,
  _args: string[],
  _options?: ExecFileOptions | null,
  _callback?: (
    error?: Error,
    stdout?: string | Buffer,
    stderr?: string | Buffer
  ) => unknown
): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('child_process.execFile');
}

export function execFileSync(
  _file: string,
  _args: string[] | ExecFileOptions | null,
  _options?: ExecFileOptions | null
): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('child_process.execFileSync');
}

export function execSync(
  _command: string,
  _options?: ExecSyncOptions | null
): Buffer | string {
  throw new ERR_METHOD_NOT_IMPLEMENTED('child_process.execSync');
}

export function fork(
  _modulePath: string | URL,
  args: readonly string[] | ForkOptions | null,
  options?: ForkOptions | null
): ChildProcess {
  if (args == null) {
    args = [];
  } else if (typeof args === 'object' && !Array.isArray(args)) {
    // @ts-expect-error TS2322 This is intentional.
    options = args;
    args = [];
  } else {
    validateArray(args, 'args');
  }

  if (options != null) {
    validateObject(options, 'options');
  }

  throw new ERR_METHOD_NOT_IMPLEMENTED('child_process.fork');
}

export function spawn(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('child_process.spawn');
}

export function spawnSync(
  command: string,
  _args?: readonly string[] | SpawnSyncOptions,
  _options?: SpawnSyncOptions
): SpawnSyncReturns<string | Buffer> {
  validateString(command, 'command');
  throw new ERR_METHOD_NOT_IMPLEMENTED('child_process.spawnSync');
}

export default {
  ChildProcess,
  _forkChild,
  exec,
  execFile,
  execFileSync,
  execSync,
  fork,
  spawn,
  spawnSync,
};
