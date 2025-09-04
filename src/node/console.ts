import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import type {
  Console as _Console,
  ConsoleConstructorOptions,
} from 'node:console';

export function Console(_options: ConsoleConstructorOptions): Console {
  throw new ERR_METHOD_NOT_IMPLEMENTED('Console');
}

export const log = console.log.bind(console);
Console.log = log;
export const info = console.info.bind(console);
Console.info = info;
export const debug = console.debug.bind(console);
Console.debug = debug;
export const warn = console.warn.bind(console);
Console.warn = warn;
export const error = console.error.bind(console);
Console.error = error;
export const dir = console.dir.bind(console);
Console.dir = dir;
export const time = console.time.bind(console);
Console.time = time;
export const timeEnd = console.timeEnd.bind(console);
Console.timeEnd = timeEnd;
export const timeLog = console.timeLog.bind(console);
Console.timeLog = timeLog;
export const trace = console.trace.bind(console);
Console.trace = trace;
export const assert = console.assert.bind(console);
Console.assert = assert;
export const clear = console.clear.bind(console);
Console.clear = clear;
export const count = console.count.bind(console);
Console.count = count;
export const countReset = console.countReset.bind(console);
Console.countReset = countReset;
export const group = console.group.bind(console);
Console.group = group;
export const groupEnd = console.groupEnd.bind(console);
Console.groupEnd = groupEnd;
export const table = console.table.bind(console);
Console.table = table;

export const dirxml = console.dirxml.bind(console);
Console.dirxml = dirxml;
export const groupCollapsed = console.groupCollapsed.bind(console);
Console.groupCollapsed = groupCollapsed;
export const profile = console.profile.bind(console);
Console.profile = profile;
export const profileEnd = console.profileEnd.bind(console);
Console.profileEnd = profileEnd;
export const timeStamp = console.timeStamp.bind(console);
Console.timeStamp = timeStamp;

export function context(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('Console.context');
}
Console.context = context;

export function createTask(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('Console.createTask');
}
Console.createTask = createTask;

// We want to make sure the following works:
// ```js
// import mod from 'node:console`;
// new mod.Console(...)
// ```
Console.Console = Console;

export default Console;
