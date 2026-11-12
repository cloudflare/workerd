// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

// The Console class from Node.js' lib/internal/console/constructor.js. It
// writes to the streams it is given. The global console is provided by the
// runtime and is not built by this class, but `instanceof Console` is still
// true for it (see Symbol.hasInstance below).

import {
  ERR_CONSOLE_WRITABLE_STREAM,
  ERR_INCOMPATIBLE_OPTION_PAIR,
} from 'node-internal:internal_errors';
import {
  validateArray,
  validateInteger,
  validateObject,
  validateOneOf,
} from 'node-internal:validators';
import {
  formatWithOptions,
  getStringWidth,
  inspect,
  isStackOverflowError,
  type InspectOptions,
} from 'node-internal:internal_inspect';
import {
  isMap,
  isMapIterator,
  isSet,
  isSetIterator,
  isTypedArray,
} from 'node-internal:internal_types';
import { Buffer } from 'node-internal:internal_buffer';
import internalUtil from 'node-internal:util';
import processImpl from 'node-internal:public_process';

interface ConsoleStream {
  write(chunk: string, cb?: (err?: Error | null) => void): unknown;
  isTTY?: boolean;
  getColorDepth?: () => number;
  listenerCount(event: string): number;
  once(event: string, listener: () => void): unknown;
  removeListener(event: string, listener: () => void): unknown;
  _writableState?: { errorEmitted?: boolean };
}

type ColorMode = 'auto' | boolean;

interface ConsoleConstructorOptions {
  stdout?: unknown;
  stderr?: unknown;
  ignoreErrors?: unknown;
  colorMode?: unknown;
  inspectOptions?: unknown;
  groupIndentation?: unknown;
}

const kMaxGroupIndentation = 1000;

const kCounts = Symbol('counts');
const kGroupIndentationWidth = Symbol('kGroupIndentWidth');
const kGroupIndentationString = Symbol('kGroupIndentationString');
const kColorMode = Symbol('kColorMode');
const kIsConsole = Symbol('kIsConsole');
const kInspectOptions = Symbol('kInspectOptions');

interface ConsoleImpl {
  _stdout: ConsoleStream;
  _stderr: ConsoleStream;
  _stdoutErrorHandler: (err?: Error | null) => void;
  _stderrErrorHandler: (err?: Error | null) => void;
  _ignoreErrors: boolean;
  _times: Map<string, number>;
  [kCounts]: Map<string, number>;
  [kColorMode]: ColorMode;
  [kIsConsole]: true;
  [kGroupIndentationWidth]: number;
  [kGroupIndentationString]: string;
  [kInspectOptions]: Map<ConsoleStream, InspectOptions> | undefined;
  log(...args: unknown[]): void;
  warn(...args: unknown[]): void;
  error(...args: unknown[]): void;
  [key: string | symbol]: unknown;
}

function noop(): void {}

// Coerces everything other than Symbol to a string, like Node.js does with a
// template literal.
function toLabel(label: unknown): string {
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-template-expression
  return `${label as string}`;
}

function isWritable(stream: unknown): stream is ConsoleStream {
  return (
    !!stream && typeof (stream as Partial<ConsoleStream>).write === 'function'
  );
}

// Mirrors Node.js' shouldColorize() without the FORCE_COLOR override.
function shouldColorize(stream: ConsoleStream): boolean {
  if (!stream.isTTY) return false;
  return typeof stream.getColorDepth === 'function'
    ? stream.getColorDepth() > 2
    : true;
}

// Node.js' Console accepts `new Console(stdout[, stderr][, ignoreErrors])` and
// `new Console(options)`, and can be called without `new`. It is written as a
// function rather than a class so that the call form keeps working.
export function Console(
  this: ConsoleImpl,
  options?: unknown,
  ...rest: unknown[]
): ConsoleImpl | undefined {
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition -- undefined when called without `new`
  if (new.target === undefined) {
    return Reflect.construct(Console, [options, ...rest]) as ConsoleImpl;
  }

  const opts: ConsoleConstructorOptions =
    !options || typeof (options as Partial<ConsoleStream>).write === 'function'
      ? { stdout: options, stderr: rest[0], ignoreErrors: rest[1] }
      : (options as ConsoleConstructorOptions);

  const {
    stdout,
    stderr = stdout,
    ignoreErrors = true,
    colorMode = 'auto',
    inspectOptions,
    groupIndentation = 2,
  } = opts;

  if (!isWritable(stdout)) {
    throw new ERR_CONSOLE_WRITABLE_STREAM('stdout');
  }
  if (!isWritable(stderr)) {
    throw new ERR_CONSOLE_WRITABLE_STREAM('stderr');
  }

  validateOneOf<unknown>(colorMode, 'colorMode', ['auto', true, false]);
  validateInteger(
    groupIndentation,
    'groupIndentation',
    0,
    kMaxGroupIndentation
  );

  let inspectOptionsMap: Map<ConsoleStream, InspectOptions> | undefined;
  if (inspectOptions !== undefined) {
    validateObject(inspectOptions, 'options.inspectOptions');
    // The options are copied because the resolved `colors` value is stored on
    // them on first use.
    inspectOptionsMap = new Map();
    if (isMap(inspectOptions)) {
      for (const [stream, streamOptions] of inspectOptions as Map<
        ConsoleStream,
        InspectOptions
      >) {
        inspectOptionsMap.set(stream, { ...streamOptions });
      }
    } else {
      inspectOptionsMap.set(stdout, { ...inspectOptions });
      inspectOptionsMap.set(stderr, { ...inspectOptions });
    }
    for (const streamOptions of inspectOptionsMap.values()) {
      if (streamOptions.colors !== undefined && opts.colorMode !== undefined) {
        throw new ERR_INCOMPATIBLE_OPTION_PAIR(
          'options.inspectOptions.color',
          'colorMode'
        );
      }
    }
  }

  // Bind the methods read from the instance, not from Console.prototype, so
  // that subclasses can override them.
  for (const key of Object.keys(Console.prototype as object)) {
    const bound = (this[key] as (...args: unknown[]) => unknown).bind(this);
    Object.defineProperty(bound, 'name', { value: key });
    this[key] = bound;
  }

  const define = (key: string | symbol, value: unknown): void => {
    Object.defineProperty(this, key, {
      value,
      writable: true,
      enumerable: false,
      configurable: true,
    });
  };
  define('_stdout', stdout);
  define('_stderr', stderr);
  define('_stdoutErrorHandler', createWriteErrorHandler(this, true));
  define('_stderrErrorHandler', createWriteErrorHandler(this, false));
  define('_ignoreErrors', Boolean(ignoreErrors));
  define('_times', new Map<string, number>());
  define(kCounts, new Map<string, number>());
  define(kColorMode, colorMode);
  define(kIsConsole, true);
  define(kGroupIndentationWidth, groupIndentation);
  define(kGroupIndentationString, '');
  define(kInspectOptions, inspectOptionsMap);
  Object.defineProperty(this, Symbol.toStringTag, {
    value: 'console',
    writable: false,
    enumerable: false,
    configurable: true,
  });
  return undefined;
}

// The global console also counts as a Console, as in Node.js.
const globalConsole = globalThis.console;
Object.defineProperty(Console, Symbol.hasInstance, {
  value: function (instance: unknown): boolean {
    return (
      instance === globalConsole ||
      (instance != null &&
        (instance as { [kIsConsole]?: unknown })[kIsConsole] === true)
    );
  },
});

function createWriteErrorHandler(
  instance: ConsoleImpl,
  useStdout: boolean
): (err?: Error | null) => void {
  return (err?: Error | null): void => {
    // This is true only for errors that the stream did not already emit, which
    // happens when the stream invokes the write callback asynchronously.
    const stream = useStdout ? instance._stdout : instance._stderr;
    if (err != null && !stream._writableState?.errorEmitted) {
      // A once listener keeps the 'error' event from becoming an uncaught
      // exception without affecting writes that do not come from the console.
      if (stream.listenerCount('error') === 0) {
        stream.once('error', noop);
      }
    }
  };
}

function writeToConsole(
  self: ConsoleImpl,
  useStdout: boolean,
  string: string
): void {
  const groupIndent = self[kGroupIndentationString];
  const stream = useStdout ? self._stdout : self._stderr;
  const errorHandler = useStdout
    ? self._stdoutErrorHandler
    : self._stderrErrorHandler;

  if (groupIndent.length !== 0) {
    if (string.includes('\n')) {
      string = string.replace(/\n/g, `\n${groupIndent}`);
    }
    string = groupIndent + string;
  }
  string += '\n';

  if (!self._ignoreErrors) {
    stream.write(string);
    return;
  }

  // Errors can be raised synchronously or asynchronously depending on the
  // stream, so both are handled.
  try {
    // A temporary noop listener catches synchronous errors.
    if (stream.listenerCount('error') === 0) stream.once('error', noop);
    stream.write(string, errorHandler);
  } catch (e) {
    // Console is a debugging utility, so it should not swallow stack overflows.
    if (isStackOverflowError(e as Error)) throw e;
  } finally {
    stream.removeListener('error', noop);
  }
}

const kColorInspectOptions: InspectOptions = { colors: true };
const kNoColorInspectOptions: InspectOptions = {};

function getInspectOptions(
  self: ConsoleImpl,
  stream: ConsoleStream
): InspectOptions {
  let color = self[kColorMode];
  if (color === 'auto') {
    color = shouldColorize(stream);
  }

  const options = self[kInspectOptions]?.get(stream);
  if (options) {
    if (options.colors === undefined) {
      options.colors = color;
    }
    return options;
  }

  return color ? kColorInspectOptions : kNoColorInspectOptions;
}

function formatFor(
  self: ConsoleImpl,
  stream: ConsoleStream,
  args: unknown[]
): string {
  if (args.length === 1 && typeof args[0] === 'string') {
    return args[0];
  }
  return formatWithOptions(getInspectOptions(self, stream), ...args);
}

function logTo(self: ConsoleImpl, useStdout: boolean, args: unknown[]): void {
  const stream = useStdout ? self._stdout : self._stderr;
  writeToConsole(self, useStdout, formatFor(self, stream, args));
}

const kSecond = 1000;
const kMinute = 60 * kSecond;
const kHour = 60 * kMinute;

function pad(value: number | string): string {
  return `${value}`.padStart(2, '0');
}

// formatTime() from Node.js' lib/internal/util/debuglog.js.
function formatTime(ms: number): string {
  let hours = 0;
  let minutes = 0;
  let seconds = 0;

  if (ms >= kSecond) {
    if (ms >= kMinute) {
      if (ms >= kHour) {
        hours = Math.floor(ms / kHour);
        ms = ms % kHour;
      }
      minutes = Math.floor(ms / kMinute);
      ms = ms % kMinute;
    }
    seconds = ms / kSecond;
  }

  if (hours !== 0 || minutes !== 0) {
    const [secondsPart, msPart] = seconds.toFixed(3).split('.', 2);
    const res = hours !== 0 ? `${hours}:${pad(minutes)}` : `${minutes}`;
    return `${res}:${pad(secondsPart as string)}.${msPart} (${hours !== 0 ? 'h:m' : ''}m:ss.mmm)`;
  }

  if (seconds !== 0) {
    return `${seconds.toFixed(3)}s`;
  }

  return `${Number(ms.toFixed(3))}ms`;
}

function timeLogImpl(
  self: ConsoleImpl,
  name: string,
  label: string,
  data?: unknown[]
): void {
  const start = self._times.get(label);
  if (start === undefined) {
    processImpl.emitWarning(`No such label '${label}' for console.${name}()`);
    return;
  }
  const formatted = formatTime(performance.now() - start);
  if (data === undefined) {
    self.log('%s: %s', label, formatted);
  } else {
    self.log('%s: %s', label, formatted, ...data);
  }
}

const keyKey = 'Key';
const valuesKey = 'Values';
const indexKey = '(index)';
const iterKey = '(iteration index)';

const isArray = (v: unknown): boolean =>
  Array.isArray(v) || isTypedArray(v) || Buffer.isBuffer(v);

// The table renderer from Node.js' lib/internal/cli_table.js. The box-drawing
// characters are escaped because built-in module sources must be ASCII.
const tableChars = {
  middleMiddle: '\u2500',
  rowMiddle: '\u253c',
  topRight: '\u2510',
  topLeft: '\u250c',
  leftMiddle: '\u251c',
  topMiddle: '\u252c',
  bottomRight: '\u2518',
  bottomLeft: '\u2514',
  bottomMiddle: '\u2534',
  rightMiddle: '\u2524',
  left: '\u2502 ',
  right: ' \u2502',
  middle: ' \u2502 ',
};

function renderRow(row: string[], columnWidths: number[]): string {
  let out = tableChars.left;
  for (let i = 0; i < row.length; i++) {
    const cell = row[i] as string;
    const needed = (columnWidths[i] as number) - getStringWidth(cell);
    out += cell + ' '.repeat(Math.ceil(needed));
    if (i !== row.length - 1) out += tableChars.middle;
  }
  out += tableChars.right;
  return out;
}

function renderTable(head: string[], columns: string[][]): string {
  const rows: string[][] = [];
  const columnWidths = head.map((h) => getStringWidth(h));
  const longestColumn = Math.max(...columns.map((a) => a.length));

  for (let i = 0; i < head.length; i++) {
    const column = columns[i] as string[];
    for (let j = 0; j < longestColumn; j++) {
      const row = (rows[j] ??= []);
      const value = (row[i] = Object.hasOwn(column, j)
        ? (column[j] as string)
        : '');
      columnWidths[i] = Math.max(columnWidths[i] ?? 0, getStringWidth(value));
    }
  }

  const divider = columnWidths.map((i) =>
    tableChars.middleMiddle.repeat(i + 2)
  );

  let result =
    tableChars.topLeft +
    divider.join(tableChars.topMiddle) +
    tableChars.topRight +
    '\n' +
    renderRow(head, columnWidths) +
    '\n' +
    tableChars.leftMiddle +
    divider.join(tableChars.rowMiddle) +
    tableChars.rightMiddle +
    '\n';

  for (const row of rows) {
    result += `${renderRow(row, columnWidths)}\n`;
  }

  result +=
    tableChars.bottomLeft +
    divider.join(tableChars.bottomMiddle) +
    tableChars.bottomRight;

  return result;
}

// Method shorthand keeps these non-constructible, like Node.js.
const consoleMethods = {
  log(this: ConsoleImpl, ...args: unknown[]): void {
    logTo(this, true, args);
  },

  info(this: ConsoleImpl, ...args: unknown[]): void {
    logTo(this, true, args);
  },

  debug(this: ConsoleImpl, ...args: unknown[]): void {
    logTo(this, true, args);
  },

  warn(this: ConsoleImpl, ...args: unknown[]): void {
    logTo(this, false, args);
  },

  error(this: ConsoleImpl, ...args: unknown[]): void {
    logTo(this, false, args);
  },

  dir(this: ConsoleImpl, object: unknown, options?: InspectOptions): void {
    writeToConsole(
      this,
      true,
      inspect(object, {
        customInspect: false,
        ...getInspectOptions(this, this._stdout),
        ...options,
      })
    );
  },

  time(this: ConsoleImpl, label: unknown = 'default'): void {
    const key = toLabel(label);
    if (this._times.has(key)) {
      processImpl.emitWarning(
        `Label '${key}' already exists for console.time()`
      );
      return;
    }
    this._times.set(key, performance.now());
  },

  timeEnd(this: ConsoleImpl, label: unknown = 'default'): void {
    const key = toLabel(label);
    timeLogImpl(this, 'timeEnd', key);
    this._times.delete(key);
  },

  timeLog(
    this: ConsoleImpl,
    label: unknown = 'default',
    ...data: unknown[]
  ): void {
    timeLogImpl(this, 'timeLog', toLabel(label), data);
  },

  trace(this: ConsoleImpl, ...args: unknown[]): void {
    const err = {
      name: 'Trace',
      message: formatFor(this, this._stderr, args),
      stack: '',
    };
    // eslint-disable-next-line @typescript-eslint/unbound-method
    Error.captureStackTrace(err, consoleMethods.trace);
    this.error(err.stack);
  },

  // https://console.spec.whatwg.org/#assert
  assert(this: ConsoleImpl, expression: unknown, ...args: unknown[]): void {
    if (!expression) {
      if (args.length && typeof args[0] === 'string') {
        args[0] = `Assertion failed: ${args[0]}`;
      } else {
        args.unshift('Assertion failed');
      }
      // The arguments are formatted by warn().
      // eslint-disable-next-line @typescript-eslint/unbound-method
      Reflect.apply(this.warn, this, args);
    }
  },

  // https://console.spec.whatwg.org/#clear
  clear(this: ConsoleImpl): void {
    // Clearing only makes sense when _stdout is a TTY. The sequences move the
    // cursor to the top left and clear the screen below it.
    if (
      this._stdout.isTTY &&
      (processImpl.env as Record<string, unknown>).TERM !== 'dumb'
    ) {
      this._stdout.write('\x1b[1;1H\x1b[0J');
    }
  },

  // https://console.spec.whatwg.org/#count
  count(this: ConsoleImpl, label: unknown = 'default'): void {
    const key = toLabel(label);
    const counts = this[kCounts];
    const count = (counts.get(key) ?? 0) + 1;
    counts.set(key, count);
    this.log(`${key}: ${count}`);
  },

  // https://console.spec.whatwg.org/#countreset
  countReset(this: ConsoleImpl, label: unknown = 'default'): void {
    const counts = this[kCounts];
    // Like Node.js, this looks up the raw label but deletes the stringified
    // one, so countReset(1) after count(1) warns.
    if (!counts.has(label as string)) {
      processImpl.emitWarning(`Count for '${label as string}' does not exist`);
      return;
    }
    counts.delete(toLabel(label));
  },

  group(this: ConsoleImpl, ...data: unknown[]): void {
    if (data.length > 0) {
      // eslint-disable-next-line @typescript-eslint/unbound-method
      Reflect.apply(this.log, this, data);
    }
    this[kGroupIndentationString] += ' '.repeat(this[kGroupIndentationWidth]);
  },

  groupEnd(this: ConsoleImpl): void {
    const currentIndentation = this[kGroupIndentationString];
    this[kGroupIndentationString] = currentIndentation.slice(
      0,
      currentIndentation.length - this[kGroupIndentationWidth]
    );
  },

  // https://console.spec.whatwg.org/#table
  table(this: ConsoleImpl, tabularData: unknown, properties?: string[]): void {
    if (properties !== undefined) validateArray(properties, 'properties');

    if (tabularData === null || typeof tabularData !== 'object') {
      this.log(tabularData);
      return;
    }

    const final = (k: string[], v: string[][]): void => {
      this.log(renderTable(k, v));
    };

    const _inspect = (v: unknown): string => {
      const depth =
        v !== null &&
        typeof v === 'object' &&
        !isArray(v) &&
        Object.keys(v).length > 2
          ? -1
          : 0;
      return inspect(v, {
        depth,
        maxArrayLength: 3,
        breakLength: Infinity,
        ...getInspectOptions(this, this._stdout),
      });
    };
    const getIndexArray = (length: number): string[] =>
      Array.from({ length }, (_, i) => _inspect(i));

    let data: unknown = tabularData;
    const mapIter = isMapIterator(data);
    let isKeyValue = false;
    if (mapIter) {
      const res = internalUtil.previewEntries(data);
      data = res?.entries ?? [];
      isKeyValue = res?.isKeyValue ?? false;
    }

    if (isKeyValue || isMap(data)) {
      const keys: string[] = [];
      const values: string[] = [];
      if (mapIter) {
        const entries = data as unknown[];
        for (let i = 0; i < entries.length / 2; ++i) {
          keys.push(_inspect(entries[i * 2]));
          values.push(_inspect(entries[i * 2 + 1]));
        }
      } else {
        for (const [k, v] of data as Map<unknown, unknown>) {
          keys.push(_inspect(k));
          values.push(_inspect(v));
        }
      }
      final(
        [iterKey, keyKey, valuesKey],
        [getIndexArray(keys.length), keys, values]
      );
      return;
    }

    const setIter = isSetIterator(data);
    if (setIter) data = internalUtil.previewEntries(data)?.entries ?? [];

    if (setIter || mapIter || isSet(data)) {
      const values: string[] = [];
      for (const v of data as Iterable<unknown>) {
        values.push(_inspect(v));
      }
      final([iterKey, valuesKey], [getIndexArray(values.length), values]);
      return;
    }

    const map: Record<string, string[]> = Object.create(null) as Record<
      string,
      string[]
    >;
    let hasPrimitives = false;
    const valuesKeyArray: string[] = [];
    const obj = data as Record<string, unknown>;
    const indexKeyArray = Object.keys(obj);

    for (let i = 0; i < indexKeyArray.length; i++) {
      const item = obj[indexKeyArray[i] as string];
      const primitive =
        item === null ||
        (typeof item !== 'function' && typeof item !== 'object');
      if (properties === undefined && primitive) {
        hasPrimitives = true;
        valuesKeyArray[i] = _inspect(item);
      } else {
        const keys = properties ?? Object.keys(item as object);
        for (const key of keys) {
          const column = (map[key] ??= []);
          if (
            (primitive && properties) ||
            !Object.hasOwn(item as object, key)
          ) {
            column[i] = '';
          } else {
            column[i] = _inspect((item as Record<string, unknown>)[key]);
          }
        }
      }
    }

    const keys = Object.keys(map);
    const values = Object.values(map);
    if (hasPrimitives) {
      keys.push(valuesKey);
      values.push(valuesKeyArray);
    }
    keys.unshift(indexKey);
    values.unshift(indexKeyArray);

    final(keys, values);
  },
};

const proto = Console.prototype as Record<string, unknown>;
for (const [key, method] of Object.entries(consoleMethods)) {
  proto[key] = method;
}
/* eslint-disable @typescript-eslint/unbound-method */
proto.dirxml = consoleMethods.log;
proto.groupCollapsed = consoleMethods.group;
/* eslint-enable @typescript-eslint/unbound-method */
