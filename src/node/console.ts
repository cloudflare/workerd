// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

/* eslint-disable @typescript-eslint/no-unsafe-member-access */

import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import processImpl from 'node-internal:public_process';
import type { Console as _Console, ConsoleOptions } from 'node:console';

const noop: VoidFunction = () => {};

// TODO(soon): This class is Node.js compatible but globalThis.console is not.
// This is because globalThis.console is provided by the v8 runtime.
// We need to find a way to patch and add createTask and context methods whenever
// `enable_nodejs_console_module` flag is enabled.
const globalConsole = globalThis.console as typeof globalThis.console & {
  _times: Map<string, number>;
  _ignoreErrors: boolean;
  _stderrErrorHandler: () => void;
  _stdoutErrorHandler: () => void;
  _stderr: NodeJS.WritableStream;
  _stdout: NodeJS.WritableStream;
};

globalConsole._times = new Map<string, number>();
globalConsole._ignoreErrors = true;
globalConsole._stderrErrorHandler = noop;
globalConsole._stdoutErrorHandler = noop;

// We have this assertion because node-internal:public_process has undefined as the type
globalConsole._stderr = processImpl.stderr as unknown as NodeJS.WritableStream;
globalConsole._stdout = processImpl.stdout as unknown as NodeJS.WritableStream;

export function Console(_options: ConsoleOptions): Console {
  throw new ERR_METHOD_NOT_IMPLEMENTED('Console');
}
// We want to make sure the following works:
// ```js
// import mod from 'node:console`;
// new mod.Console(...)
// ```
// @ts-expect-error TS2322 This is intentional.
globalConsole.Console = Console;

export const log = console.log.bind(console);
Console.prototype.log = log;
export const info = console.info.bind(console);
Console.prototype.info = info;
export const debug = console.debug.bind(console);
Console.prototype.debug = debug;
export const warn = console.warn.bind(console);
Console.prototype.warn = warn;
export const error = console.error.bind(console);
Console.prototype.error = error;
export const dir = console.dir.bind(console);
Console.prototype.dir = dir;
export const time = console.time.bind(console);
Console.prototype.time = time;
export const timeEnd = console.timeEnd.bind(console);
Console.prototype.timeEnd = timeEnd;
export const timeLog = console.timeLog.bind(console);
Console.prototype.timeLog = timeLog;
export const trace = console.trace.bind(console);
Console.prototype.trace = trace;
export const assert = console.assert.bind(console);
Console.prototype.assert = assert;
export const clear = console.clear.bind(console);
Console.prototype.clear = clear;
export const count = console.count.bind(console);
Console.prototype.count = count;
export const countReset = console.countReset.bind(console);
Console.prototype.countReset = countReset;
export const group = console.group.bind(console);
Console.prototype.group = group;
export const groupEnd = console.groupEnd.bind(console);
Console.prototype.groupEnd = groupEnd;
export const table = console.table.bind(console);
Console.prototype.table = table;

export const dirxml = console.dirxml.bind(console);
Console.prototype.dirxml = dirxml;
export const groupCollapsed = console.groupCollapsed.bind(console);
Console.prototype.groupCollapsed = groupCollapsed;
export const profile = console.profile.bind(console);
Console.prototype.profile = profile;
export const profileEnd = console.profileEnd.bind(console);
Console.prototype.profileEnd = profileEnd;
export const timeStamp = console.timeStamp.bind(console);
Console.prototype.timeStamp = timeStamp;

export function context(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('Console.context');
}
// @ts-expect-error TS2339 This is intentional.
globalConsole.context = context;
Console.prototype.context = context;

export function createTask(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('Console.createTask');
}
// @ts-expect-error TS2339 This is intentional.
globalConsole.createTask = createTask;
Console.prototype.createTask = createTask;

export default globalConsole;
