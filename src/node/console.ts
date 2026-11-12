// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import processImpl from 'node-internal:public_process';
import { Console } from 'node-internal:internal_console';

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

export { Console };
// We want to make sure the following works:
// ```js
// import mod from 'node:console`;
// new mod.Console(...)
// ```
// @ts-expect-error TS2322 This is intentional.
globalConsole.Console = Console;

export const log = console.log.bind(console);
export const info = console.info.bind(console);
export const debug = console.debug.bind(console);
export const warn = console.warn.bind(console);
export const error = console.error.bind(console);
export const dir = console.dir.bind(console);
export const time = console.time.bind(console);
export const timeEnd = console.timeEnd.bind(console);
export const timeLog = console.timeLog.bind(console);
export const trace = console.trace.bind(console);
export const assert = console.assert.bind(console);
export const clear = console.clear.bind(console);
export const count = console.count.bind(console);
export const countReset = console.countReset.bind(console);
export const group = console.group.bind(console);
export const groupEnd = console.groupEnd.bind(console);
export const table = console.table.bind(console);

export const dirxml = console.dirxml.bind(console);
export const groupCollapsed = console.groupCollapsed.bind(console);
export const profile = console.profile.bind(console);
export const profileEnd = console.profileEnd.bind(console);
export const timeStamp = console.timeStamp.bind(console);

export function context(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('Console.context');
}
// @ts-expect-error TS2339 This is intentional.
globalConsole.context = context;

export function createTask(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('Console.createTask');
}
// @ts-expect-error TS2339 This is intentional.
globalConsole.createTask = createTask;

export default globalConsole;
