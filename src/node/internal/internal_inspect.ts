// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Deno, Node.js and DefinitelyTyped:
// Copyright 2018-2022 the Deno authors. All rights reserved. MIT license.
//
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

/* TODO: the following is adopted code, enabling linting one day */
/* eslint-disable */

import internalWorkers from 'cloudflare-internal:workers';
import internal from 'node-internal:util';

import { Buffer } from 'node-internal:internal_buffer';
import {
  isAsyncFunction,
  isGeneratorFunction,
  isAnyArrayBuffer,
  isArrayBuffer,
  isArgumentsObject,
  isBoxedPrimitive,
  isDataView,
  isMap,
  isMapIterator,
  isModuleNamespaceObject,
  isNativeError,
  isPromise,
  isSet,
  isSetIterator,
  isWeakMap,
  isWeakSet,
  isRegExp,
  isDate,
  isTypedArray,
  isStringObject,
  isNumberObject,
  isBooleanObject,
  isBigIntObject,
} from 'node-internal:internal_types';
// import { ALL_PROPERTIES, ONLY_ENUMERABLE, getOwnNonIndexProperties } from "node-internal:internal_utils";
import {
  validateObject,
  validateString,
  kValidateObjectAllowArray,
} from 'node-internal:validators';

// Simplified assertions to avoid `Assertions require every name in the call target to be
// declared with an explicit type` TypeScript error
function assert(value: boolean, message = 'Assertion failed'): asserts value {
  if (!value) throw new Error(message);
}
assert.fail = function (message = 'Assertion failed'): never {
  throw new Error(message);
};

function isError(e: unknown): e is Error {
  // An error could be an instance of Error while not being a native error
  // or could be from a different realm and not be instance of Error but still
  // be a native error.
  return isNativeError(e) || e instanceof Error;
}

const typedArrayPrototype = Object.getPrototypeOf(Uint8Array).prototype;
const typedArrayPrototypeLength: (this: NodeJS.TypedArray) => number =
  Object.getOwnPropertyDescriptor(typedArrayPrototype, 'length')!.get!;
const typedArrayPrototypeToStringTag: (this: NodeJS.TypedArray) => string =
  Object.getOwnPropertyDescriptor(
    typedArrayPrototype,
    Symbol.toStringTag
  )!.get!;

const setPrototypeSize: (this: Set<unknown>) => number =
  Object.getOwnPropertyDescriptor(Set.prototype, 'size')!.get!;
const mapPrototypeSize: (this: Map<unknown, unknown>) => number =
  Object.getOwnPropertyDescriptor(Map.prototype, 'size')!.get!;

let maxStack_ErrorName: string;
let maxStack_ErrorMessage: string;
function isStackOverflowError(err: Error): boolean {
  if (maxStack_ErrorMessage === undefined) {
    try {
      function overflowStack() {
        overflowStack();
      }
      overflowStack();
    } catch (err) {
      assert(isError(err));
      maxStack_ErrorMessage = err.message;
      maxStack_ErrorName = err.name;
    }
  }

  return (
    err &&
    err.name === maxStack_ErrorName &&
    err.message === maxStack_ErrorMessage
  );
}

export const customInspectSymbol = Symbol.for('nodejs.util.inspect.custom');

const colorRegExp = /\u001b\[\d\d?m/g;

function removeColors(str: string): string {
  return str.replace(colorRegExp, '');
}

export interface InspectOptions {
  /**
   * If `true`, object's non-enumerable symbols and properties are included in the formatted result.
   * `WeakMap` and `WeakSet` entries are also included as well as user defined prototype properties (excluding method properties).
   * @default false
   */
  showHidden?: boolean;
  /**
   * Specifies the number of times to recurse while formatting object.
   * This is useful for inspecting large objects.
   * To recurse up to the maximum call stack size pass `Infinity` or `null`.
   * @default 2
   */
  depth?: number | null;
  /**
   * If `true`, the output is styled with ANSI color codes. Colors are customizable.
   */
  colors?: boolean;
  /**
   * If `false`, `[util.inspect.custom](depth, opts, inspect)` functions are not invoked.
   * @default true
   */
  customInspect?: boolean;
  /**
   * If `true`, `Proxy` inspection includes the target and handler objects.
   * @default false
   */
  showProxy?: boolean;
  /**
   * Specifies the maximum number of `Array`, `TypedArray`, `WeakMap`, and `WeakSet` elements
   * to include when formatting. Set to `null` or `Infinity` to show all elements.
   * Set to `0` or negative to show no elements.
   * @default 100
   */
  maxArrayLength?: number | null;
  /**
   * Specifies the maximum number of characters to
   * include when formatting. Set to `null` or `Infinity` to show all elements.
   * Set to `0` or negative to show no characters.
   * @default 10000
   */
  maxStringLength?: number | null;
  /**
   * The length at which input values are split across multiple lines.
   * Set to `Infinity` to format the input as a single line
   * (in combination with `compact` set to `true` or any number >= `1`).
   * @default 80
   */
  breakLength?: number;
  /**
   * Setting this to `false` causes each object key
   * to be displayed on a new line. It will also add new lines to text that is
   * longer than `breakLength`. If set to a number, the most `n` inner elements
   * are united on a single line as long as all properties fit into
   * `breakLength`. Short array elements are also grouped together. Note that no
   * text will be reduced below 16 characters, no matter the `breakLength` size.
   * For more information, see the example below.
   * @default true
   */
  compact?: boolean | number;
  /**
   * If set to `true` or a function, all properties of an object, and `Set` and `Map`
   * entries are sorted in the resulting string.
   * If set to `true` the default sort is used.
   * If set to a function, it is used as a compare function.
   */
  sorted?: boolean | ((a: string, b: string) => number);
  /**
   * If set to `true`, getters are going to be
   * inspected as well. If set to `'get'` only getters without setter are going
   * to be inspected. If set to `'set'` only getters having a corresponding
   * setter are going to be inspected. This might cause side effects depending on
   * the getter function.
   * @default false
   */
  getters?: 'get' | 'set' | boolean;
  /**
   * If set to `true`, an underscore is used to separate every three digits in all bigints and numbers.
   * @default false
   */
  numericSeparator?: boolean;
}
export type Style =
  | 'special'
  | 'number'
  | 'bigint'
  | 'boolean'
  | 'undefined'
  | 'null'
  | 'string'
  | 'symbol'
  | 'date'
  | 'regexp'
  | 'module'
  | 'name';
export type CustomInspectFunction = (
  depth: number,
  options: InspectOptionsStylized
) => any; // TODO: , inspect: inspect
export interface InspectOptionsStylized extends InspectOptions {
  stylize(text: string, styleType: Style): string;
}

const builtInObjects = new Set(
  Object.getOwnPropertyNames(globalThis).filter(
    (e) => /^[A-Z][a-zA-Z0-9]+$/.exec(e) !== null
  )
);

// https://tc39.es/ecma262/#sec-IsHTMLDDA-internal-slot
const isUndetectableObject = (v: unknown): boolean =>
  typeof v === 'undefined' && v !== undefined;

// These options must stay in sync with `getUserOptions`. So if any option will
// be added or removed, `getUserOptions` must also be updated accordingly.
export const inspectDefaultOptions = Object.seal({
  showHidden: false,
  depth: 2,
  colors: false,
  customInspect: true,
  showProxy: false,
  maxArrayLength: 100,
  maxStringLength: 10000,
  breakLength: 80,
  compact: 3,
  sorted: false,
  getters: false,
  numericSeparator: false,
} as const);

const kObjectType = 0;
const kArrayType = 1;
const kArrayExtrasType = 2;

const strEscapeSequencesRegExp =
  /[\x00-\x1f\x27\x5c\x7f-\x9f]|[\ud800-\udbff](?![\udc00-\udfff])|(?<![\ud800-\udbff])[\udc00-\udfff]/;
const strEscapeSequencesReplacer =
  /[\x00-\x1f\x27\x5c\x7f-\x9f]|[\ud800-\udbff](?![\udc00-\udfff])|(?<![\ud800-\udbff])[\udc00-\udfff]/g;
const strEscapeSequencesRegExpSingle =
  /[\x00-\x1f\x5c\x7f-\x9f]|[\ud800-\udbff](?![\udc00-\udfff])|(?<![\ud800-\udbff])[\udc00-\udfff]/;
const strEscapeSequencesReplacerSingle =
  /[\x00-\x1f\x5c\x7f-\x9f]|[\ud800-\udbff](?![\udc00-\udfff])|(?<![\ud800-\udbff])[\udc00-\udfff]/g;

const keyStrRegExp = /^[a-zA-Z_][a-zA-Z_0-9]*$/;
const numberRegExp = /^(0|[1-9][0-9]*)$/;

const nodeModulesRegExp = /[/\\]node_modules[/\\](.+?)(?=[/\\])/g;

const classRegExp = /^(\s+[^(]*?)\s*{/;
// eslint-disable-next-line node-core/no-unescaped-regexp-dot
const stripCommentsRegExp = /(\/\/.*?\n)|(\/\*(.|\n)*?\*\/)/g;

const kMinLineLength = 16;

// Constants to map the iterator state.
const kWeak = 0;
const kIterator = 1;
const kMapEntries = 2;

// Escaped control characters (plus the single quote and the backslash). Use
// empty strings to fill up unused entries.
const meta = [
  '\\x00',
  '\\x01',
  '\\x02',
  '\\x03',
  '\\x04',
  '\\x05',
  '\\x06',
  '\\x07', // x07
  '\\b',
  '\\t',
  '\\n',
  '\\x0B',
  '\\f',
  '\\r',
  '\\x0E',
  '\\x0F', // x0F
  '\\x10',
  '\\x11',
  '\\x12',
  '\\x13',
  '\\x14',
  '\\x15',
  '\\x16',
  '\\x17', // x17
  '\\x18',
  '\\x19',
  '\\x1A',
  '\\x1B',
  '\\x1C',
  '\\x1D',
  '\\x1E',
  '\\x1F', // x1F
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  "\\'",
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '', // x2F
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '', // x3F
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '', // x4F
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '\\\\',
  '',
  '',
  '', // x5F
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '', // x6F
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '',
  '\\x7F', // x7F
  '\\x80',
  '\\x81',
  '\\x82',
  '\\x83',
  '\\x84',
  '\\x85',
  '\\x86',
  '\\x87', // x87
  '\\x88',
  '\\x89',
  '\\x8A',
  '\\x8B',
  '\\x8C',
  '\\x8D',
  '\\x8E',
  '\\x8F', // x8F
  '\\x90',
  '\\x91',
  '\\x92',
  '\\x93',
  '\\x94',
  '\\x95',
  '\\x96',
  '\\x97', // x97
  '\\x98',
  '\\x99',
  '\\x9A',
  '\\x9B',
  '\\x9C',
  '\\x9D',
  '\\x9E',
  '\\x9F', // x9F
];

// Regex used for ansi escape code splitting
// Adopted from https://github.com/chalk/ansi-regex/blob/HEAD/index.js
// License: MIT, authors: @sindresorhus, Qix-, arjunmehta and LitoMore
// Matches all ansi escape code sequences in a string
const ansiPattern = new RegExp(
  '[\\u001B\\u009B][[\\]()#;?]*' +
    '(?:(?:(?:(?:;[-a-zA-Z\\d\\/\\#&.:=?%@~_]+)*' +
    '|[a-zA-Z\\d]+(?:;[-a-zA-Z\\d\\/\\#&.:=?%@~_]*)*)?' +
    '(?:\\u0007|\\u001B\\u005C|\\u009C))' +
    '|(?:(?:\\d{1,4}(?:;\\d{0,4})*)?' +
    '[\\dA-PR-TZcf-nq-uy=><~]))',
  'g'
);
const ansi = new RegExp(ansiPattern, 'g');

interface Context extends Required<InspectOptionsStylized> {
  maxArrayLength: number;
  maxStringLength: number;
  budget: Record<string, number>;
  indentationLvl: number;
  seen: unknown[];
  currentDepth: number;
  userOptions?: InspectOptions;
  circular?: Map<unknown, number>;
}

function getUserOptions(
  ctx: Context,
  isCrossContext: boolean
): InspectOptionsStylized {
  const ret: InspectOptionsStylized = {
    stylize: ctx.stylize,
    showHidden: ctx.showHidden,
    depth: ctx.depth,
    colors: ctx.colors,
    customInspect: ctx.customInspect,
    showProxy: ctx.showProxy,
    maxArrayLength: ctx.maxArrayLength,
    maxStringLength: ctx.maxStringLength,
    breakLength: ctx.breakLength,
    compact: ctx.compact,
    sorted: ctx.sorted,
    getters: ctx.getters,
    numericSeparator: ctx.numericSeparator,
    ...ctx.userOptions,
  };

  // Typically, the target value will be an instance of `Object`. If that is
  // *not* the case, the object may come from another vm.Context, and we want
  // to avoid passing it objects from this Context in that case, so we remove
  // the prototype from the returned object itself + the `stylize()` function,
  // and remove all other non-primitives, including non-primitive user options.
  if (isCrossContext) {
    Object.setPrototypeOf(ret, null);
    for (const key of Object.keys(ret) as (keyof InspectOptionsStylized)[]) {
      if (
        (typeof ret[key] === 'object' || typeof ret[key] === 'function') &&
        ret[key] !== null
      ) {
        delete ret[key];
      }
    }
    ret.stylize = Object.setPrototypeOf((value: string, flavour: Style) => {
      let stylized;
      try {
        stylized = `${ctx.stylize(value, flavour)}`;
      } catch {
        // Continue regardless of error.
      }

      if (typeof stylized !== 'string') return value;
      // `stylized` is a string as it should be, which is safe to pass along.
      return stylized;
    }, null);
  }

  return ret;
}

/**
 * Echos the value of any input. Tries to print the value out
 * in the best way possible given the different types.
 * @param {any} value The value to print out.
 * @param {object} opts Optional options object that alters the output.
 */
/* Legacy: value, showHidden, depth, colors */
export function inspect(
  value: unknown,
  showHidden?: boolean,
  depth?: number | null,
  color?: boolean
): string;
export function inspect(value: unknown, opts?: InspectOptions): string;
export function inspect(
  value: unknown,
  opts?: Partial<InspectOptionsStylized> | boolean
): string {
  // Default options
  const ctx: Context = {
    budget: {},
    indentationLvl: 0,
    seen: [],
    currentDepth: 0,
    stylize: stylizeNoColor,
    showHidden: inspectDefaultOptions.showHidden,
    depth: inspectDefaultOptions.depth,
    colors: inspectDefaultOptions.colors,
    customInspect: inspectDefaultOptions.customInspect,
    showProxy: inspectDefaultOptions.showProxy,
    maxArrayLength: inspectDefaultOptions.maxArrayLength,
    maxStringLength: inspectDefaultOptions.maxStringLength,
    breakLength: inspectDefaultOptions.breakLength,
    compact: inspectDefaultOptions.compact,
    sorted: inspectDefaultOptions.sorted,
    getters: inspectDefaultOptions.getters,
    numericSeparator: inspectDefaultOptions.numericSeparator,
  };
  if (arguments.length > 1) {
    // Legacy...
    if (arguments.length > 2) {
      if (arguments[2] !== undefined) {
        ctx.depth = arguments[2];
      }
      if (arguments.length > 3 && arguments[3] !== undefined) {
        ctx.colors = arguments[3];
      }
    }
    // Set user-specified options
    if (typeof opts === 'boolean') {
      ctx.showHidden = opts;
    } else if (opts) {
      const optKeys = Object.keys(opts) as (keyof InspectOptionsStylized)[];
      for (let i = 0; i < optKeys.length; ++i) {
        const key = optKeys[i]!;
        // TODO(BridgeAR): Find a solution what to do about stylize. Either make
        // this function public or add a new API with a similar or better
        // functionality.
        if (
          Object.prototype.hasOwnProperty.call(inspectDefaultOptions, key) ||
          key === 'stylize'
        ) {
          (ctx as Record<keyof InspectOptionsStylized, unknown>)[key] =
            opts[key];
        } else if (ctx.userOptions === undefined) {
          // This is required to pass through the actual user input.
          ctx.userOptions = opts;
        }
      }
    }
  }
  if (ctx.colors) ctx.stylize = stylizeWithColor;
  if (ctx.maxArrayLength === null) ctx.maxArrayLength = Infinity;
  if (ctx.maxStringLength === null) ctx.maxStringLength = Infinity;
  return formatValue(ctx, value, 0);
}
inspect.custom = customInspectSymbol;

Object.defineProperty(inspect, 'defaultOptions', {
  get() {
    return inspectDefaultOptions;
  },
  set(options) {
    validateObject(options, 'options');
    return Object.assign(inspectDefaultOptions, options);
  },
});

// Set Graphics Rendition https://en.wikipedia.org/wiki/ANSI_escape_code#graphics
// Each color consists of an array with the color code as first entry and the
// reset code as second entry.
const defaultFG = 39;
const defaultBG = 49;
const colors: Record<string, [number, number]> = {
  // @ts-ignore
  __proto__: null,
  reset: [0, 0],
  bold: [1, 22],
  dim: [2, 22], // Alias: faint
  italic: [3, 23],
  underline: [4, 24],
  blink: [5, 25],
  // Swap foreground and background colors
  inverse: [7, 27], // Alias: swapcolors, swapColors
  hidden: [8, 28], // Alias: conceal
  strikethrough: [9, 29], // Alias: strikeThrough, crossedout, crossedOut
  doubleunderline: [21, 24], // Alias: doubleUnderline
  black: [30, defaultFG],
  red: [31, defaultFG],
  green: [32, defaultFG],
  yellow: [33, defaultFG],
  blue: [34, defaultFG],
  magenta: [35, defaultFG],
  cyan: [36, defaultFG],
  white: [37, defaultFG],
  bgBlack: [40, defaultBG],
  bgRed: [41, defaultBG],
  bgGreen: [42, defaultBG],
  bgYellow: [43, defaultBG],
  bgBlue: [44, defaultBG],
  bgMagenta: [45, defaultBG],
  bgCyan: [46, defaultBG],
  bgWhite: [47, defaultBG],
  framed: [51, 54],
  overlined: [53, 55],
  gray: [90, defaultFG], // Alias: grey, blackBright
  redBright: [91, defaultFG],
  greenBright: [92, defaultFG],
  yellowBright: [93, defaultFG],
  blueBright: [94, defaultFG],
  magentaBright: [95, defaultFG],
  cyanBright: [96, defaultFG],
  whiteBright: [97, defaultFG],
  bgGray: [100, defaultBG], // Alias: bgGrey, bgBlackBright
  bgRedBright: [101, defaultBG],
  bgGreenBright: [102, defaultBG],
  bgYellowBright: [103, defaultBG],
  bgBlueBright: [104, defaultBG],
  bgMagentaBright: [105, defaultBG],
  bgCyanBright: [106, defaultBG],
  bgWhiteBright: [107, defaultBG],
};
inspect.colors = colors;

function defineColorAlias(target: string, alias: string) {
  Object.defineProperty(inspect.colors, alias, {
    get() {
      return this[target];
    },
    set(value) {
      this[target] = value;
    },
    configurable: true,
    enumerable: false,
  });
}

defineColorAlias('gray', 'grey');
defineColorAlias('gray', 'blackBright');
defineColorAlias('bgGray', 'bgGrey');
defineColorAlias('bgGray', 'bgBlackBright');
defineColorAlias('dim', 'faint');
defineColorAlias('strikethrough', 'crossedout');
defineColorAlias('strikethrough', 'strikeThrough');
defineColorAlias('strikethrough', 'crossedOut');
defineColorAlias('hidden', 'conceal');
defineColorAlias('inverse', 'swapColors');
defineColorAlias('inverse', 'swapcolors');
defineColorAlias('doubleunderline', 'doubleUnderline');

// TODO(BridgeAR): Add function style support for more complex styles.
// Don't use 'blue' not visible on cmd.exe
inspect.styles = {
  __proto__: null,
  special: 'cyan',
  number: 'yellow',
  bigint: 'yellow',
  boolean: 'yellow',
  undefined: 'grey',
  null: 'bold',
  string: 'green',
  symbol: 'green',
  date: 'magenta',
  name: undefined,
  // TODO(BridgeAR): Highlight regular expressions properly.
  regexp: 'red',
  module: 'underline',
};

function addQuotes(str: string, quotes: number): string {
  if (quotes === -1) {
    return `"${str}"`;
  }
  if (quotes === -2) {
    return `\`${str}\``;
  }
  return `'${str}'`;
}

function escapeFn(str: string): string {
  const charCode = str.charCodeAt(0);
  return meta.length > charCode
    ? meta[charCode]!
    : `\\u${charCode.toString(16)}`;
}

// Escape control characters, single quotes and the backslash.
// This is similar to JSON stringify escaping.
function strEscape(str: string): string {
  let escapeTest = strEscapeSequencesRegExp;
  let escapeReplace = strEscapeSequencesReplacer;
  let singleQuote = 39;

  // Check for double quotes. If not present, do not escape single quotes and
  // instead wrap the text in double quotes. If double quotes exist, check for
  // backticks. If they do not exist, use those as fallback instead of the
  // double quotes.
  if (str.includes("'")) {
    // This invalidates the charCode and therefore can not be matched for
    // anymore.
    if (!str.includes('"')) {
      singleQuote = -1;
    } else if (!str.includes('`') && !str.includes('${')) {
      singleQuote = -2;
    }
    if (singleQuote !== 39) {
      escapeTest = strEscapeSequencesRegExpSingle;
      escapeReplace = strEscapeSequencesReplacerSingle;
    }
  }

  // Some magic numbers that worked out fine while benchmarking with v8 6.0
  if (str.length < 5000 && escapeTest.exec(str) === null)
    return addQuotes(str, singleQuote);
  if (str.length > 100) {
    str = str.replace(escapeReplace, escapeFn);
    return addQuotes(str, singleQuote);
  }

  let result = '';
  let last = 0;
  for (let i = 0; i < str.length; i++) {
    const point = str.charCodeAt(i);
    if (
      point === singleQuote ||
      point === 92 ||
      point < 32 ||
      (point > 126 && point < 160)
    ) {
      if (last === i) {
        result += meta[point];
      } else {
        result += `${str.slice(last, i)}${meta[point]}`;
      }
      last = i + 1;
    } else if (point >= 0xd800 && point <= 0xdfff) {
      if (point <= 0xdbff && i + 1 < str.length) {
        const point = str.charCodeAt(i + 1);
        if (point >= 0xdc00 && point <= 0xdfff) {
          i++;
          continue;
        }
      }
      result += `${str.slice(last, i)}\\u${point.toString(16)}`;
      last = i + 1;
    }
  }

  if (last !== str.length) {
    result += str.slice(last);
  }
  return addQuotes(result, singleQuote);
}

function stylizeWithColor(str: string, styleType: Style): string {
  const style = inspect.styles[styleType];
  if (style !== undefined) {
    const color = (
      inspect.colors as unknown as Record<string, number[] | undefined>
    )[style];
    if (color !== undefined)
      return `\u001b[${color[0]}m${str}\u001b[${color[1]}m`;
  }
  return str;
}

function stylizeNoColor(str: string): string {
  return str;
}

// Return a new empty array to push in the results of the default formatter.
function getEmptyFormatArray(): string[] {
  return [];
}

function isInstanceof(object: unknown, proto: Function): boolean {
  try {
    return object instanceof proto;
  } catch {
    return false;
  }
}

// Special-case for some builtin prototypes in case their `constructor` property has been tampered.
const wellKnownPrototypes = new Map()
  .set(Array.prototype, { name: 'Array', constructor: Array })
  .set(ArrayBuffer.prototype, { name: 'ArrayBuffer', constructor: ArrayBuffer })
  .set(Function.prototype, { name: 'Function', constructor: Function })
  .set(Map.prototype, { name: 'Map', constructor: Map })
  .set(Set.prototype, { name: 'Set', constructor: Set })
  .set(Object.prototype, { name: 'Object', constructor: Object })
  .set(Object.getPrototypeOf(Uint8Array).prototype, {
    name: 'TypedArray',
    constructor: Object.getPrototypeOf(Uint8Array),
  })
  .set(RegExp.prototype, { name: 'RegExp', constructor: RegExp })
  .set(Date.prototype, { name: 'Date', constructor: Date })
  .set(DataView.prototype, { name: 'DataView', constructor: DataView })
  .set(Error.prototype, { name: 'Error', constructor: Error })
  .set(Boolean.prototype, { name: 'Boolean', constructor: Boolean })
  .set(Number.prototype, { name: 'Number', constructor: Number })
  .set(String.prototype, { name: 'String', constructor: String })
  .set(Promise.prototype, { name: 'Promise', constructor: Promise })
  .set(WeakMap.prototype, { name: 'WeakMap', constructor: WeakMap })
  .set(WeakSet.prototype, { name: 'WeakSet', constructor: WeakSet });

function getConstructorName(
  obj: object,
  ctx: Context,
  recurseTimes: number,
  protoProps?: string[]
): string | null {
  let firstProto: unknown;
  const tmp = obj;
  while (obj || isUndetectableObject(obj)) {
    const wellKnownPrototypeNameAndConstructor = wellKnownPrototypes.get(obj);
    if (wellKnownPrototypeNameAndConstructor !== undefined) {
      const { name, constructor } = wellKnownPrototypeNameAndConstructor;
      if (Function.prototype[Symbol.hasInstance].call(constructor, tmp)) {
        if (protoProps !== undefined && firstProto !== obj) {
          addPrototypeProperties(
            ctx,
            tmp,
            firstProto || tmp,
            recurseTimes,
            protoProps
          );
        }
        return name;
      }
    }
    const descriptor = Object.getOwnPropertyDescriptor(obj, 'constructor');
    if (
      descriptor !== undefined &&
      typeof descriptor.value === 'function' &&
      descriptor.value.name !== '' &&
      isInstanceof(tmp, descriptor.value)
    ) {
      if (
        protoProps !== undefined &&
        (firstProto !== obj || !builtInObjects.has(descriptor.value.name))
      ) {
        addPrototypeProperties(
          ctx,
          tmp,
          firstProto || tmp,
          recurseTimes,
          protoProps
        );
      }
      return String(descriptor.value.name);
    }

    obj = Object.getPrototypeOf(obj);
    if (firstProto === undefined) {
      firstProto = obj;
    }
  }

  if (firstProto === null) {
    return null;
  }

  const res = internal.getConstructorName(tmp);

  if (ctx.depth !== null && recurseTimes > ctx.depth) {
    return `${res} <Complex prototype>`;
  }

  const protoConstr = getConstructorName(
    firstProto!,
    ctx,
    recurseTimes + 1,
    protoProps
  );

  if (protoConstr === null) {
    return `${res} <${inspect(firstProto, {
      ...ctx,
      customInspect: false,
      depth: -1,
    })}>`;
  }

  return `${res} <${protoConstr}>`;
}

// This function has the side effect of adding prototype properties to the
// `output` argument (which is an array). This is intended to highlight user
// defined prototype properties.
function addPrototypeProperties(
  ctx: Context,
  main: object,
  obj: Object,
  recurseTimes: number,
  output: string[]
): void {
  let depth = 0;
  let keys: PropertyKey[] | undefined;
  let keySet: Set<PropertyKey> | undefined;
  do {
    if (depth !== 0 || main === obj) {
      obj = Object.getPrototypeOf(obj);
      // Stop as soon as a null prototype is encountered.
      if (obj === null) {
        return;
      }
      // Stop as soon as a built-in object type is detected.
      const descriptor = Object.getOwnPropertyDescriptor(obj, 'constructor');
      if (
        descriptor !== undefined &&
        typeof descriptor.value === 'function' &&
        builtInObjects.has(descriptor.value.name)
      ) {
        return;
      }
    }

    if (depth === 0) {
      keySet = new Set();
    } else {
      keys!.forEach((key) => keySet!.add(key));
    }
    // Get all own property names and symbols.
    keys = Reflect.ownKeys(obj);
    ctx.seen.push(main);
    for (const key of keys) {
      // Ignore the `constructor` property and keys that exist on layers above.
      if (
        key === 'constructor' ||
        Object.prototype.hasOwnProperty.call(main, key) ||
        (depth !== 0 && keySet!.has(key))
      ) {
        continue;
      }
      const desc = Object.getOwnPropertyDescriptor(obj, key);
      if (typeof desc?.value === 'function') {
        continue;
      }
      const value = formatProperty(
        ctx,
        obj,
        recurseTimes,
        key,
        kObjectType,
        desc,
        main
      );
      if (ctx.colors) {
        // Faint!
        output.push(`\u001b[2m${value}\u001b[22m`);
      } else {
        output.push(value);
      }
    }
    ctx.seen.pop();
    // Limit the inspection to up to three prototype layers. Using `recurseTimes`
    // is not a good choice here, because it's as if the properties are declared
    // on the current object from the users perspective.
  } while (++depth !== 3);
}

function getPrefix(
  constructor: string | null,
  tag: string,
  fallback: string,
  size = ''
): string {
  if (constructor === null) {
    if (tag !== '' && fallback !== tag) {
      return `[${fallback}${size}: null prototype] [${tag}] `;
    }
    return `[${fallback}${size}: null prototype] `;
  }

  if (tag !== '' && constructor !== tag) {
    return `${constructor}${size} [${tag}] `;
  }
  return `${constructor}${size} `;
}

// Look up the keys of the object.
function getKeys(value: object, showHidden: boolean): PropertyKey[] {
  let keys: PropertyKey[];
  const symbols = Object.getOwnPropertySymbols(value);
  if (showHidden) {
    keys = Object.getOwnPropertyNames(value);
    if (symbols.length !== 0) keys.push(...symbols);
  } else {
    // This might throw if `value` is a Module Namespace Object from an
    // unevaluated module, but we don't want to perform the actual type
    // check because it's expensive.
    // TODO(devsnek): track https://github.com/tc39/ecma262/issues/1209
    // and modify this logic as needed.
    try {
      keys = Object.keys(value);
    } catch (err: any) {
      assert(
        isNativeError(err) &&
          err.name === 'ReferenceError' &&
          isModuleNamespaceObject(value)
      );
      keys = Object.getOwnPropertyNames(value);
    }
    if (symbols.length !== 0) {
      const filter = (key: PropertyKey) =>
        Object.prototype.propertyIsEnumerable.call(value, key);
      keys.push(...symbols.filter(filter));
    }
  }
  return keys;
}

function getCtxStyle(
  value: unknown,
  constructor: string | null,
  tag: string
): string {
  let fallback = '';
  if (constructor === null) {
    fallback = internal.getConstructorName(value);
    if (fallback === tag) {
      fallback = 'Object';
    }
  }
  return getPrefix(constructor, tag, fallback);
}

function formatProxy(
  ctx: Context,
  proxy: internal.ProxyDetails,
  recurseTimes: number
): string {
  if (ctx.depth !== null && recurseTimes > ctx.depth) {
    return ctx.stylize('Proxy [Array]', 'special');
  }
  recurseTimes += 1;
  ctx.indentationLvl += 2;
  const res: string[] = [
    formatValue(ctx, proxy.target, recurseTimes),
    formatValue(ctx, proxy.handler, recurseTimes),
  ];
  ctx.indentationLvl -= 2;
  return reduceToSingleString(
    ctx,
    res,
    '',
    ['Proxy [', ']'],
    kArrayExtrasType,
    recurseTimes
  );
}

// Note: using `formatValue` directly requires the indentation level to be
// corrected by setting `ctx.indentationLvL += diff` and then to decrease the
// value afterwards again.
function formatValue(
  ctx: Context,
  value: unknown,
  recurseTimes: number,
  typedArray?: unknown
): string {
  // Primitive types cannot have properties.
  if (
    typeof value !== 'object' &&
    typeof value !== 'function' &&
    !isUndetectableObject(value)
  ) {
    return formatPrimitive(ctx.stylize, value as Primitive, ctx);
  }
  if (value === null) {
    return ctx.stylize('null', 'null');
  }

  // Memorize the context for custom inspection on proxies.
  const context = value;
  let proxies = 0;
  // Always check for proxies to prevent side effects and to prevent triggering
  // any proxy handlers.
  let proxy = internal.getProxyDetails(value);
  if (proxy !== undefined) {
    if (proxy === null || proxy.target === null) {
      return ctx.stylize('<Revoked Proxy>', 'special');
    }
    if (ctx.showProxy) {
      return formatProxy(ctx, proxy, recurseTimes);
    }
    do {
      if (proxy === null || proxy.target === null) {
        let formatted = ctx.stylize('<Revoked Proxy>', 'special');
        for (let i = 0; i < proxies; i++) {
          formatted = `${ctx.stylize('Proxy(', 'special')}${formatted}${ctx.stylize(')', 'special')}`;
        }
        return formatted;
      }
      value = proxy.target;
      proxy = internal.getProxyDetails(value);
      proxies += 1;
    } while (proxy !== undefined);
  }

  // Provide a hook for user-specified inspect functions.
  // Check that value is an object with an inspect function on it.
  if (ctx.customInspect) {
    let maybeCustom = (value as Record<PropertyKey, unknown>)[
      customInspectSymbol
    ];

    // WORKERD SPECIFIC PATCH: if `value` is a JSG resource type, use a well-known custom inspect
    const maybeResourceTypeInspect = (value as Record<PropertyKey, unknown>)[
      internal.kResourceTypeInspect
    ];
    if (typeof maybeResourceTypeInspect === 'object') {
      maybeCustom = formatJsgResourceType.bind(
        context as Record<PropertyKey, unknown>,
        maybeResourceTypeInspect as Record<string, symbol>
      );
    }

    if (
      typeof maybeCustom === 'function' &&
      // Filter out the util module, its inspect function is special.
      maybeCustom !== inspect &&
      // Also filter out any prototype objects using the circular check.
      !(
        (value as object).constructor &&
        (value as object).constructor.prototype === value
      )
    ) {
      // This makes sure the recurseTimes are reported as before while using
      // a counter internally.
      const depth = ctx.depth === null ? null : ctx.depth - recurseTimes;
      const isCrossContext = proxies !== 0 || !(context instanceof Object);
      const ret = Function.prototype.call.call(
        maybeCustom,
        context,
        depth,
        getUserOptions(ctx, isCrossContext),
        inspect
      );
      // If the custom inspection method returned `this`, don't go into
      // infinite recursion.
      if (ret !== context) {
        if (typeof ret !== 'string') {
          return formatValue(ctx, ret, recurseTimes);
        }
        return ret.replaceAll('\n', `\n${' '.repeat(ctx.indentationLvl)}`);
      }
    }
  }

  // Using an array here is actually better for the average case than using
  // a Set. `seen` will only check for the depth and will never grow too large.
  if (ctx.seen.includes(value)) {
    let index: number | undefined = 1;
    if (ctx.circular === undefined) {
      ctx.circular = new Map();
      ctx.circular.set(value, index);
    } else {
      index = ctx.circular.get(value);
      if (index === undefined) {
        index = ctx.circular.size + 1;
        ctx.circular.set(value, index);
      }
    }
    return ctx.stylize(`[Circular *${index}]`, 'special');
  }

  let formatted = formatRaw(ctx, value, recurseTimes, typedArray);

  if (proxies !== 0) {
    for (let i = 0; i < proxies; i++) {
      formatted = `${ctx.stylize('Proxy(', 'special')}${formatted}${ctx.stylize(')', 'special')}`;
    }
  }

  return formatted;
}

function formatRaw(
  ctx: Context,
  value: unknown,
  recurseTimes: number,
  typedArray: unknown
): string {
  let keys: PropertyKey[] | undefined;
  let protoProps: string[] | undefined;
  if (ctx.showHidden && (ctx.depth === null || recurseTimes <= ctx.depth)) {
    protoProps = [];
  }

  const constructor = getConstructorName(
    value as object,
    ctx,
    recurseTimes,
    protoProps
  );
  // Reset the variable to check for this later on.
  if (protoProps !== undefined && protoProps.length === 0) {
    protoProps = undefined;
  }

  let tag = (value as { [Symbol.toStringTag]?: string })[Symbol.toStringTag];
  // Only list the tag in case it's non-enumerable / not an own property.
  // Otherwise we'd print this twice.
  if (
    typeof tag !== 'string' ||
    (tag !== '' &&
      (ctx.showHidden
        ? Object.prototype.hasOwnProperty
        : Object.prototype.propertyIsEnumerable
      ).call(value, Symbol.toStringTag))
  ) {
    tag = '';
  }
  let base = '';
  let formatter: (ctx: Context, value: any, recurseTimes: number) => string[] =
    getEmptyFormatArray;
  let braces: [string, string] | undefined;
  let noIterator = true;
  let i = 0;
  const filter = ctx.showHidden
    ? internal.ALL_PROPERTIES
    : internal.ONLY_ENUMERABLE;

  let extrasType = kObjectType;

  // Iterators and the rest are split to reduce checks.
  // We have to check all values in case the constructor is set to null.
  // Otherwise it would not possible to identify all types properly.

  const isEntriesObject = hasEntries(value);
  if (
    Symbol.iterator in (value as object) ||
    constructor === null ||
    isEntriesObject
  ) {
    noIterator = false;
    if (isEntriesObject) {
      // WORKERD SPECIFIC PATCH: if `value` is an object with entries, format them like a map
      const size = value[kEntries].length;
      const prefix = getPrefix(constructor, tag, 'Object', `(${size})`);
      keys = getKeys(value, ctx.showHidden);

      // Remove `kEntries` and `size` from keys
      keys.splice(keys.indexOf(kEntries), 1);
      const sizeIndex = keys.indexOf('size');
      if (sizeIndex !== -1) keys.splice(sizeIndex, 1);

      formatter = formatMap.bind(null, value[kEntries][Symbol.iterator]());
      if (size === 0 && keys.length === 0 && protoProps === undefined)
        return `${prefix}{}`;
      braces = [`${prefix}{`, '}'];
    } else if (Array.isArray(value)) {
      // Only set the constructor for non ordinary ("Array [...]") arrays.
      const prefix =
        constructor !== 'Array' || tag !== ''
          ? getPrefix(constructor, tag, 'Array', `(${value.length})`)
          : '';
      keys = internal.getOwnNonIndexProperties(value, filter);
      braces = [`${prefix}[`, ']'];
      if (value.length === 0 && keys.length === 0 && protoProps === undefined)
        return `${braces[0]}]`;
      extrasType = kArrayExtrasType;
      formatter = formatArray;
    } else if (isSet(value)) {
      const size = setPrototypeSize.call(value);
      const prefix = getPrefix(constructor, tag, 'Set', `(${size})`);
      keys = getKeys(value, ctx.showHidden);
      formatter =
        constructor !== null
          ? formatSet.bind(null, value)
          : formatSet.bind(null, Set.prototype.values.call(value));
      if (size === 0 && keys.length === 0 && protoProps === undefined)
        return `${prefix}{}`;
      braces = [`${prefix}{`, '}'];
    } else if (isMap(value)) {
      const size = mapPrototypeSize.call(value);
      const prefix = getPrefix(constructor, tag, 'Map', `(${size})`);
      keys = getKeys(value, ctx.showHidden);
      formatter =
        constructor !== null
          ? formatMap.bind(null, value)
          : formatMap.bind(null, Map.prototype.entries.call(value));
      if (size === 0 && keys.length === 0 && protoProps === undefined)
        return `${prefix}{}`;
      braces = [`${prefix}{`, '}'];
    } else if (isTypedArray(value)) {
      keys = internal.getOwnNonIndexProperties(value, filter);
      let bound = value;
      let fallback = '';
      if (constructor === null) {
        fallback = typedArrayPrototypeToStringTag.call(value);
        // Reconstruct the array information.
        bound = new (
          globalThis as unknown as Record<
            string,
            { new (value: NodeJS.TypedArray): NodeJS.TypedArray }
          >
        )[fallback]!(value);
      }
      const size = typedArrayPrototypeLength.call(value);
      const prefix = getPrefix(constructor, tag, fallback, `(${size})`);
      braces = [`${prefix}[`, ']'];
      if (value.length === 0 && keys.length === 0 && !ctx.showHidden)
        return `${braces[0]}]`;
      // Special handle the value. The original value is required below. The
      // bound function is required to reconstruct missing information.
      formatter = formatTypedArray.bind(null, bound, size);
      extrasType = kArrayExtrasType;
    } else if (isMapIterator(value)) {
      keys = getKeys(value, ctx.showHidden);
      braces = getIteratorBraces('Map', tag);
      // Add braces to the formatter parameters.
      formatter = formatIterator.bind(null, braces);
    } else if (isSetIterator(value)) {
      keys = getKeys(value, ctx.showHidden);
      braces = getIteratorBraces('Set', tag);
      // Add braces to the formatter parameters.
      formatter = formatIterator.bind(null, braces);
    } else {
      noIterator = true;
    }
  }
  if (noIterator) {
    keys = getKeys(value as object, ctx.showHidden);
    braces = ['{', '}'];
    if (constructor === 'Object') {
      if (isArgumentsObject(value)) {
        braces[0] = '[Arguments] {';
      } else if (tag !== '') {
        braces[0] = `${getPrefix(constructor, tag, 'Object')}{`;
      }
      if (keys.length === 0 && protoProps === undefined) {
        return `${braces[0]}}`;
      }
    } else if (typeof value === 'function') {
      base = getFunctionBase(ctx, value, constructor, tag);
      if (keys.length === 0 && protoProps === undefined)
        return ctx.stylize(base, 'special');
    } else if (isRegExp(value)) {
      // Make RegExps say that they are RegExps
      base = RegExp.prototype.toString.call(
        constructor !== null ? value : new RegExp(value)
      );
      const prefix = getPrefix(constructor, tag, 'RegExp');
      if (prefix !== 'RegExp ') base = `${prefix}${base}`;
      if (
        (keys.length === 0 && protoProps === undefined) ||
        (ctx.depth !== null && recurseTimes > ctx.depth)
      ) {
        return ctx.stylize(base, 'regexp');
      }
    } else if (isDate(value)) {
      // Make dates with properties first say the date
      base = Number.isNaN(Date.prototype.getTime.call(value))
        ? Date.prototype.toString.call(value)
        : Date.prototype.toISOString.call(value);
      const prefix = getPrefix(constructor, tag, 'Date');
      if (prefix !== 'Date ') base = `${prefix}${base}`;
      if (keys.length === 0 && protoProps === undefined) {
        return ctx.stylize(base, 'date');
      }
    } else if (isError(value)) {
      base = formatError(value, constructor, tag, ctx, keys);
      if (keys.length === 0 && protoProps === undefined) return base;
    } else if (isAnyArrayBuffer(value)) {
      // Fast path for ArrayBuffer and SharedArrayBuffer.
      // Can't do the same for DataView because it has a non-primitive
      // .buffer property that we need to recurse for.
      const arrayType = isArrayBuffer(value)
        ? 'ArrayBuffer'
        : 'SharedArrayBuffer';
      const prefix = getPrefix(constructor, tag, arrayType);
      if (typedArray === undefined) {
        formatter = formatArrayBuffer;
      } else if (keys.length === 0 && protoProps === undefined) {
        return (
          prefix +
          `{ byteLength: ${formatNumber(ctx.stylize, value.byteLength, false)} }`
        );
      }
      braces[0] = `${prefix}{`;
      keys.unshift('byteLength');
    } else if (isDataView(value)) {
      braces[0] = `${getPrefix(constructor, tag, 'DataView')}{`;
      // .buffer goes last, it's not a primitive like the others.
      keys.unshift('byteLength', 'byteOffset', 'buffer');
    } else if (isPromise(value)) {
      braces[0] = `${getPrefix(constructor, tag, 'Promise')}{`;
      formatter = formatPromise;
    } else if (isWeakSet(value)) {
      braces[0] = `${getPrefix(constructor, tag, 'WeakSet')}{`;
      formatter = ctx.showHidden ? formatWeakSet : formatWeakCollection;
    } else if (isWeakMap(value)) {
      braces[0] = `${getPrefix(constructor, tag, 'WeakMap')}{`;
      formatter = ctx.showHidden ? formatWeakMap : formatWeakCollection;
    } else if (isModuleNamespaceObject(value)) {
      braces[0] = `${getPrefix(constructor, tag, 'Module')}{`;
      // Special handle keys for namespace objects.
      formatter = formatNamespaceObject.bind(null, keys);
    } else if (isBoxedPrimitive(value)) {
      base = getBoxedBase(value, ctx, keys, constructor, tag);
      if (keys.length === 0 && protoProps === undefined) {
        return base;
      }
    } else {
      if (keys.length === 0 && protoProps === undefined) {
        return `${getCtxStyle(value, constructor, tag)}{}`;
      }
      braces[0] = `${getCtxStyle(value, constructor, tag)}{`;
    }
  }

  if (ctx.depth !== null && recurseTimes > ctx.depth) {
    let constructorName = getCtxStyle(value, constructor, tag).slice(0, -1);
    if (constructor !== null) constructorName = `[${constructorName}]`;
    return ctx.stylize(constructorName, 'special');
  }
  recurseTimes += 1;

  ctx.seen.push(value);
  ctx.currentDepth = recurseTimes;
  let output;
  const indentationLvl = ctx.indentationLvl;
  try {
    output = formatter(ctx, value, recurseTimes);
    for (i = 0; i < keys!.length; i++) {
      output.push(
        formatProperty(
          ctx,
          value as object,
          recurseTimes,
          keys![i]!,
          extrasType
        )
      );
    }
    if (protoProps !== undefined) {
      output.push(...protoProps);
    }
  } catch (err) {
    const constructorName = getCtxStyle(value, constructor, tag).slice(0, -1);
    return handleMaxCallStackSize(
      ctx,
      err as Error,
      constructorName,
      indentationLvl
    );
  }
  if (ctx.circular !== undefined) {
    const index = ctx.circular.get(value);
    if (index !== undefined) {
      const reference = ctx.stylize(`<ref *${index}>`, 'special');
      // Add reference always to the very beginning of the output.
      if (ctx.compact !== true) {
        base = base === '' ? reference : `${reference} ${base}`;
      } else {
        braces![0] = `${reference} ${braces![0]}`;
      }
    }
  }
  ctx.seen.pop();

  if (ctx.sorted) {
    const comparator = ctx.sorted === true ? undefined : ctx.sorted;
    if (extrasType === kObjectType) {
      output.sort(comparator);
    } else if (keys!.length > 1) {
      const sorted = output
        .slice(output.length - keys!.length)
        .sort(comparator);
      output.splice(output.length - keys!.length, keys!.length, ...sorted);
    }
  }

  const res = reduceToSingleString(
    ctx,
    output,
    base,
    braces!,
    extrasType,
    recurseTimes,
    value
  );
  const budget = ctx.budget[ctx.indentationLvl] || 0;
  const newLength = budget + res.length;
  ctx.budget[ctx.indentationLvl] = newLength;
  // If any indentationLvl exceeds this limit, limit further inspecting to the
  // minimum. Otherwise the recursive algorithm might continue inspecting the
  // object even though the maximum string size (~2 ** 28 on 32 bit systems and
  // ~2 ** 30 on 64 bit systems) exceeded. The actual output is not limited at
  // exactly 2 ** 27 but a bit higher. This depends on the object shape.
  // This limit also makes sure that huge objects don't block the event loop
  // significantly.
  if (newLength > 2 ** 27) {
    ctx.depth = -1;
  }
  return res;
}

function getIteratorBraces(type: string, tag: string): [string, string] {
  if (tag !== `${type} Iterator`) {
    if (tag !== '') tag += '] [';
    tag += `${type} Iterator`;
  }
  return [`[${tag}] {`, '}'];
}

function getBoxedBase(
  value: unknown,
  ctx: Context,
  keys: PropertyKey[],
  constructor: string | null,
  tag: string
): string {
  let fn: (this: unknown) => Primitive;
  let type: Capitalize<Exclude<Style, 'bigint'>> | 'BigInt';
  if (isNumberObject(value)) {
    fn = Number.prototype.valueOf;
    type = 'Number';
  } else if (isStringObject(value)) {
    fn = String.prototype.valueOf;
    type = 'String';
    // For boxed Strings, we have to remove the 0-n indexed entries,
    // since they just noisy up the output and are redundant
    // Make boxed primitive Strings look like such
    keys.splice(0, value.length);
  } else if (isBooleanObject(value)) {
    fn = Boolean.prototype.valueOf;
    type = 'Boolean';
  } else if (isBigIntObject(value)) {
    fn = BigInt.prototype.valueOf;
    type = 'BigInt';
  } else {
    fn = Symbol.prototype.valueOf;
    type = 'Symbol';
  }
  let base = `[${type}`;
  if (type !== constructor) {
    if (constructor === null) {
      base += ' (null prototype)';
    } else {
      base += ` (${constructor})`;
    }
  }
  base += `: ${formatPrimitive(stylizeNoColor, fn.call(value), ctx)}]`;
  if (tag !== '' && tag !== constructor) {
    base += ` [${tag}]`;
  }
  if (keys.length !== 0 || ctx.stylize === stylizeNoColor) return base;
  return ctx.stylize(base, type.toLowerCase() as Style);
}

function getClassBase(
  value: any,
  constructor: string | null,
  tag: string
): string {
  const hasName = Object.prototype.hasOwnProperty.call(value, 'name');
  const name = (hasName && value.name) || '(anonymous)';
  let base = `class ${name}`;
  if (constructor !== 'Function' && constructor !== null) {
    base += ` [${constructor}]`;
  }
  if (tag !== '' && constructor !== tag) {
    base += ` [${tag}]`;
  }
  if (constructor !== null) {
    const superName = Object.getPrototypeOf(value).name;
    if (superName) {
      base += ` extends ${superName}`;
    }
  } else {
    base += ' extends [null prototype]';
  }
  return `[${base}]`;
}

function getFunctionBase(
  ctx: Context,
  value: Function,
  constructor: string | null,
  tag: string
): string {
  const stringified = Function.prototype.toString.call(value);
  if (stringified.startsWith('class') && stringified.endsWith('}')) {
    const slice = stringified.slice(5, -1);
    const bracketIndex = slice.indexOf('{');
    if (
      bracketIndex !== -1 &&
      (!slice.slice(0, bracketIndex).includes('(') ||
        // Slow path to guarantee that it's indeed a class.
        classRegExp.exec(slice.replace(stripCommentsRegExp, '')) !== null)
    ) {
      return getClassBase(value, constructor, tag);
    }
  }
  let type = 'Function';
  if (isGeneratorFunction(value)) {
    type = `Generator${type}`;
  }
  if (isAsyncFunction(value)) {
    type = `Async${type}`;
  }
  let base = `[${type}`;
  if (constructor === null) {
    base += ' (null prototype)';
  }
  if (value.name === '') {
    base += ' (anonymous)';
  } else {
    base += `: ${typeof value.name === 'string' ? value.name : formatValue(ctx, value.name, NaN)}`;
  }
  base += ']';
  if (constructor !== type && constructor !== null) {
    base += ` ${constructor}`;
  }
  if (tag !== '' && constructor !== tag) {
    base += ` [${tag}]`;
  }
  return base;
}

export function identicalSequenceRange(
  a: unknown[],
  b: unknown[]
): { len: number; offset: number } {
  for (let i = 0; i < a.length - 3; i++) {
    // Find the first entry of b that matches the current entry of a.
    const pos = b.indexOf(a[i]);
    if (pos !== -1) {
      const rest = b.length - pos;
      if (rest > 3) {
        let len = 1;
        const maxLen = Math.min(a.length - i, rest);
        // Count the number of consecutive entries.
        while (maxLen > len && a[i + len] === b[pos + len]) {
          len++;
        }
        if (len > 3) {
          return { len, offset: i };
        }
      }
    }
  }

  return { len: 0, offset: 0 };
}

function getStackString(ctx: Context, error: Error): string {
  if (error.stack) {
    if (typeof error.stack === 'string') {
      return error.stack;
    }
    // This 'NaN' is a very strange Nodeism, but is necessary for correct behaviour!
    return formatValue(ctx, error.stack, NaN);
  }
  return Error.prototype.toString.call(error);
}

function getStackFrames(ctx: Context, err: Error, stack: string): string[] {
  const frames = stack.split('\n');

  let cause;
  try {
    ({ cause } = err);
  } catch {
    // If 'cause' is a getter that throws, ignore it.
  }

  // Remove stack frames identical to frames in cause.
  if (cause != null && isError(cause)) {
    const causeStack = getStackString(ctx, cause);
    const causeStackStart = causeStack.indexOf('\n    at');
    if (causeStackStart !== -1) {
      const causeFrames = causeStack.slice(causeStackStart + 1).split('\n');
      const { len, offset } = identicalSequenceRange(frames, causeFrames);
      if (len > 0) {
        const skipped = len - 2;
        const msg = `    ... ${skipped} lines matching cause stack trace ...`;
        frames.splice(offset + 1, skipped, ctx.stylize(msg, 'undefined'));
      }
    }
  }
  return frames;
}

function improveStack(
  stack: string,
  constructor: string | null,
  name: string | object,
  tag: string
): string {
  if (typeof name !== 'string') {
    stack = stack.replace(
      `${name}`,
      `${name} [${getPrefix(constructor, tag, 'Error').slice(0, -1)}]`
    );
  }

  // A stack trace may contain arbitrary data. Only manipulate the output
  // for "regular errors" (errors that "look normal") for now.
  let len = typeof name === 'string' ? name.length : undefined;

  if (
    constructor === null ||
    (typeof name === 'string' &&
      name.endsWith('Error') &&
      stack.startsWith(name) &&
      (stack.length === len ||
        stack[len as number] === ':' ||
        stack[len as number] === '\n'))
  ) {
    let fallback = 'Error';
    if (constructor === null) {
      const start =
        /^([A-Z][a-z_ A-Z0-9[\]()-]+)(?::|\n {4}at)/.exec(stack) ||
        /^([a-z_A-Z0-9-]*Error)$/.exec(stack);
      fallback = (start && start[1]) || '';
      len = fallback.length;
      fallback = fallback || 'Error';
    }
    const prefix = getPrefix(constructor, tag, fallback).slice(0, -1);
    if (name !== prefix) {
      if (typeof name === 'string' && prefix.includes(name)) {
        if (len === 0) {
          stack = `${prefix}: ${stack}`;
        } else {
          stack = `${prefix}${stack.slice(len)}`;
        }
      } else {
        stack = `${prefix} [${name}]${stack.slice(len)}`;
      }
    }
  }
  return stack;
}

function removeDuplicateErrorKeys(
  ctx: Context,
  keys: PropertyKey[],
  err: Error,
  stack: string
): void {
  if (!ctx.showHidden && keys.length !== 0) {
    for (const name of ['name', 'message', 'stack'] as const) {
      const index = keys.indexOf(name);
      // Only hide the property in case it's part of the original stack
      if (
        index !== -1 &&
        (typeof err[name] !== 'string' || stack.includes(err[name]!))
      ) {
        keys.splice(index, 1);
      }
    }
  }
}

function markNodeModules(ctx: Context, line: string): string {
  let tempLine = '';
  let nodeModule;
  let pos = 0;
  while ((nodeModule = nodeModulesRegExp.exec(line)) !== null) {
    // '/node_modules/'.length === 14
    tempLine += line.slice(pos, nodeModule.index + 14);
    tempLine += ctx.stylize(nodeModule[1]!, 'module');
    pos = nodeModule.index + nodeModule[0].length;
  }
  if (pos !== 0) {
    line = tempLine + line.slice(pos);
  }
  return line;
}

function formatError(
  err: Error,
  constructor: string | null,
  tag: string,
  ctx: Context,
  keys: PropertyKey[]
): string {
  const name = err.name != null ? (err.name as string | object) : 'Error';
  let stack = getStackString(ctx, err);

  removeDuplicateErrorKeys(ctx, keys, err, stack);

  if ('cause' in err && (keys.length === 0 || !keys.includes('cause'))) {
    keys.push('cause');
  }

  // Print errors aggregated into AggregateError
  if (
    Array.isArray((err as { errors?: unknown }).errors) &&
    (keys.length === 0 || !keys.includes('errors'))
  ) {
    keys.push('errors');
  }

  stack = improveStack(stack, constructor, name, tag);

  // Ignore the error message if it's contained in the stack.
  let pos = (err.message && stack.indexOf(err.message)) || -1;
  if (pos !== -1) pos += err.message.length;
  // Wrap the error in brackets in case it has no stack trace.
  const stackStart = stack.indexOf('\n    at', pos);
  if (stackStart === -1) {
    stack = `[${stack}]`;
  } else {
    let newStack = stack.slice(0, stackStart);
    const stackFramePart = stack.slice(stackStart + 1);
    const lines = getStackFrames(ctx, err, stackFramePart);
    if (ctx.colors) {
      // Highlight userland code and node modules.
      for (let line of lines) {
        newStack += '\n';

        line = markNodeModules(ctx, line);

        newStack += line;
      }
    } else {
      newStack += `\n${lines.join('\n')}`;
    }
    stack = newStack;
  }
  // The message and the stack have to be indented as well!
  if (ctx.indentationLvl !== 0) {
    const indentation = ' '.repeat(ctx.indentationLvl);
    stack = stack.replaceAll('\n', `\n${indentation}`);
  }
  return stack;
}

function groupArrayElements(
  ctx: Context,
  output: string[],
  value: unknown[] | undefined
): string[] {
  let totalLength = 0;
  let maxLength = 0;
  let i = 0;
  let outputLength = output.length;
  if (ctx.maxArrayLength !== null && ctx.maxArrayLength < output.length) {
    // This makes sure the "... n more items" part is not taken into account.
    outputLength--;
  }
  const separatorSpace = 2; // Add 1 for the space and 1 for the separator.
  const dataLen = Array.from<number>({ length: outputLength });
  // Calculate the total length of all output entries and the individual max
  // entries length of all output entries. We have to remove colors first,
  // otherwise the length would not be calculated properly.
  for (; i < outputLength; i++) {
    const len = getStringWidth(output[i] as string, ctx.colors);
    dataLen[i] = len;
    totalLength += len + separatorSpace;
    if (maxLength < len) maxLength = len;
  }
  // Add two to `maxLength` as we add a single whitespace character plus a comma
  // in-between two entries.
  const actualMax = maxLength + separatorSpace;
  // Check if at least three entries fit next to each other and prevent grouping
  // of arrays that contains entries of very different length (i.e., if a single
  // entry is longer than 1/5 of all other entries combined). Otherwise the
  // space in-between small entries would be enormous.
  if (
    actualMax * 3 + ctx.indentationLvl < ctx.breakLength &&
    (totalLength / actualMax > 5 || maxLength <= 6)
  ) {
    const approxCharHeights = 2.5;
    const averageBias = Math.sqrt(actualMax - totalLength / output.length);
    const biasedMax = Math.max(actualMax - 3 - averageBias, 1);
    // Dynamically check how many columns seem possible.
    const columns = Math.min(
      // Ideally a square should be drawn. We expect a character to be about 2.5
      // times as high as wide. This is the area formula to calculate a square
      // which contains n rectangles of size `actualMax * approxCharHeights`.
      // Divide that by `actualMax` to receive the correct number of columns.
      // The added bias increases the columns for short entries.
      Math.round(
        Math.sqrt(approxCharHeights * biasedMax * outputLength) / biasedMax
      ),
      // Do not exceed the breakLength.
      Math.floor((ctx.breakLength - ctx.indentationLvl) / actualMax),
      // Limit array grouping for small `compact` modes as the user requested
      // minimal grouping.
      (ctx.compact === false
        ? 0
        : ctx.compact === true
          ? inspectDefaultOptions.compact
          : ctx.compact) * 4,
      // Limit the columns to a maximum of fifteen.
      15
    );
    // Return with the original output if no grouping should happen.
    if (columns <= 1) {
      return output;
    }
    const tmp: string[] = [];
    const maxLineLength: number[] = [];
    for (let i = 0; i < columns; i++) {
      let lineMaxLength = 0;
      for (let j = i; j < output.length; j += columns) {
        if ((dataLen[j] as number) > lineMaxLength) {
          lineMaxLength = dataLen[j] as number;
        }
      }
      lineMaxLength += separatorSpace;
      maxLineLength[i] = lineMaxLength;
    }
    let order = String.prototype.padStart;
    if (value !== undefined) {
      for (let i = 0; i < output.length; i++) {
        if (typeof value[i] !== 'number' && typeof value[i] !== 'bigint') {
          order = String.prototype.padEnd;
          break;
        }
      }
    }
    // Each iteration creates a single line of grouped entries.
    for (let i = 0; i < outputLength; i += columns) {
      // The last lines may contain less entries than columns.
      const max = Math.min(i + columns, outputLength);
      let str = '';
      let j = i;
      for (; j < max - 1; j++) {
        // Calculate extra color padding in case it's active. This has to be
        // done line by line as some lines might contain more colors than
        // others.
        const padding =
          maxLineLength[j - i]! + output[j]!.length - (dataLen[j] as number);
        str += order.call(`${output[j]}, `, padding, ' ');
      }
      if (order === String.prototype.padStart) {
        const padding =
          maxLineLength[j - i]! +
          output[j]!.length -
          (dataLen[j] as number) -
          separatorSpace;
        str += output[j]!.padStart(padding, ' ');
      } else {
        str += output[j];
      }
      tmp.push(str);
    }
    if (ctx.maxArrayLength !== null && ctx.maxArrayLength < output.length) {
      tmp.push(output[outputLength]!);
    }
    output = tmp;
  }
  return output;
}

function handleMaxCallStackSize(
  ctx: Context,
  err: Error,
  constructorName: string,
  indentationLvl: number
): string {
  if (isStackOverflowError(err)) {
    ctx.seen.pop();
    ctx.indentationLvl = indentationLvl;
    return ctx.stylize(
      `[${constructorName}: Inspection interrupted ` +
        'prematurely. Maximum call stack size exceeded.]',
      'special'
    );
  }
  /* c8 ignore next */
  assert.fail(err.stack);
}

function addNumericSeparator(integerString: string): string {
  let result = '';
  let i = integerString.length;
  const start = integerString.startsWith('-') ? 1 : 0;
  for (; i >= start + 4; i -= 3) {
    result = `_${integerString.slice(i - 3, i)}${result}`;
  }
  return i === integerString.length
    ? integerString
    : `${integerString.slice(0, i)}${result}`;
}

function addNumericSeparatorEnd(integerString: string): string {
  let result = '';
  let i = 0;
  for (; i < integerString.length - 3; i += 3) {
    result += `${integerString.slice(i, i + 3)}_`;
  }
  return i === 0 ? integerString : `${result}${integerString.slice(i)}`;
}

const remainingText = (remaining: number) =>
  `... ${remaining} more item${remaining > 1 ? 's' : ''}`;

function formatNumber(
  fn: InspectOptionsStylized['stylize'],
  number: number,
  numericSeparator?: boolean
): string {
  if (!numericSeparator) {
    // Format -0 as '-0'. Checking `number === -0` won't distinguish 0 from -0.
    if (Object.is(number, -0)) {
      return fn('-0', 'number');
    }
    return fn(`${number}`, 'number');
  }
  const integer = Math.trunc(number);
  const string = String(integer);
  if (integer === number) {
    if (!Number.isFinite(number) || string.includes('e')) {
      return fn(string, 'number');
    }
    return fn(`${addNumericSeparator(string)}`, 'number');
  }
  if (Number.isNaN(number)) {
    return fn(string, 'number');
  }
  return fn(
    `${addNumericSeparator(string)}.${addNumericSeparatorEnd(
      String(number).slice(string.length + 1)
    )}`,
    'number'
  );
}

function formatBigInt(
  fn: InspectOptionsStylized['stylize'],
  bigint: bigint,
  numericSeparator?: boolean
): string {
  const string = String(bigint);
  if (!numericSeparator) {
    return fn(`${string}n`, 'bigint');
  }
  return fn(`${addNumericSeparator(string)}n`, 'bigint');
}

type Primitive = string | number | bigint | boolean | undefined | symbol;
function formatPrimitive(
  fn: InspectOptionsStylized['stylize'],
  value: Primitive,
  ctx: Context
): string {
  if (typeof value === 'string') {
    let trailer = '';
    if (ctx.maxStringLength !== null && value.length > ctx.maxStringLength) {
      const remaining = value.length - ctx.maxStringLength;
      value = value.slice(0, ctx.maxStringLength);
      trailer = `... ${remaining} more character${remaining > 1 ? 's' : ''}`;
    }
    if (
      ctx.compact !== true &&
      // We do not support handling Unicode characters width with
      // the readline getStringWidth function as there are
      // performance implications.
      value.length > kMinLineLength &&
      value.length > ctx.breakLength - ctx.indentationLvl - 4
    ) {
      return (
        value
          .split(/(?<=\n)/)
          .map((line) => fn(strEscape(line), 'string'))
          .join(` +\n${' '.repeat(ctx.indentationLvl + 2)}`) + trailer
      );
    }
    return fn(strEscape(value), 'string') + trailer;
  }
  if (typeof value === 'number')
    return formatNumber(fn, value, ctx.numericSeparator);
  if (typeof value === 'bigint')
    return formatBigInt(fn, value, ctx.numericSeparator);
  if (typeof value === 'boolean') return fn(`${value}`, 'boolean');
  if (typeof value === 'undefined') return fn('undefined', 'undefined');
  // es6 symbol primitive
  return fn(Symbol.prototype.toString.call(value), 'symbol');
}

function formatNamespaceObject(
  keys: PropertyKey[],
  ctx: Context,
  value: object,
  recurseTimes: number
): string[] {
  const output = new Array<string>(keys.length);
  for (let i = 0; i < keys.length; i++) {
    try {
      output[i] = formatProperty(
        ctx,
        value,
        recurseTimes,
        keys[i]!,
        kObjectType
      );
    } catch (err) {
      assert(isNativeError(err) && err.name === 'ReferenceError');
      // Use the existing functionality. This makes sure the indentation and
      // line breaks are always correct. Otherwise it is very difficult to keep
      // this aligned, even though this is a hacky way of dealing with this.
      const tmp = { [keys[i]!]: '' };
      output[i] = formatProperty(ctx, tmp, recurseTimes, keys[i]!, kObjectType);
      const pos = output[i]!.lastIndexOf(' ');
      // We have to find the last whitespace and have to replace that value as
      // it will be visualized as a regular string.
      output[i] =
        output[i]!.slice(0, pos + 1) +
        ctx.stylize('<uninitialized>', 'special');
    }
  }
  // Reset the keys to an empty array. This prevents duplicated inspection.
  keys.length = 0;
  return output;
}

// The array is sparse and/or has extra keys
function formatSpecialArray(
  ctx: Context,
  value: unknown[],
  recurseTimes: number,
  maxLength: number,
  output: string[],
  i: number
): string[] {
  const keys = Object.keys(value);
  let index = i;
  for (; i < keys.length && output.length < maxLength; i++) {
    const key = keys[i]!;
    const tmp = +key;
    // Arrays can only have up to 2^32 - 1 entries
    if (tmp > 2 ** 32 - 2) {
      break;
    }
    if (`${index}` !== key) {
      if (numberRegExp.exec(key) === null) {
        break;
      }
      const emptyItems = tmp - index;
      const ending = emptyItems > 1 ? 's' : '';
      const message = `<${emptyItems} empty item${ending}>`;
      output.push(ctx.stylize(message, 'undefined'));
      index = tmp;
      if (output.length === maxLength) {
        break;
      }
    }
    output.push(formatProperty(ctx, value, recurseTimes, key, kArrayType));
    index++;
  }
  const remaining = value.length - index;
  if (output.length !== maxLength) {
    if (remaining > 0) {
      const ending = remaining > 1 ? 's' : '';
      const message = `<${remaining} empty item${ending}>`;
      output.push(ctx.stylize(message, 'undefined'));
    }
  } else if (remaining > 0) {
    output.push(remainingText(remaining));
  }
  return output;
}

function formatArrayBuffer(ctx: Context, value: ArrayBuffer): string[] {
  let buffer;
  try {
    buffer = new Uint8Array(value);
  } catch {
    return [ctx.stylize('(detached)', 'special')];
  }
  const maxArrayLength = ctx.maxArrayLength;
  let str = Buffer.prototype.hexSlice
    .call(buffer, 0, Math.min(maxArrayLength, buffer.length))
    .replace(/(.{2})/g, '$1 ')
    .trim();
  const remaining = buffer.length - maxArrayLength;
  if (remaining > 0)
    str += ` ... ${remaining} more byte${remaining > 1 ? 's' : ''}`;
  return [`${ctx.stylize('[Uint8Contents]', 'special')}: <${str}>`];
}

function formatArray(
  ctx: Context,
  value: unknown[],
  recurseTimes: number
): string[] {
  const valLen = value.length;
  const len = Math.min(Math.max(0, ctx.maxArrayLength), valLen);

  const remaining = valLen - len;
  const output: string[] = [];
  for (let i = 0; i < len; i++) {
    // Special handle sparse arrays.
    if (!Object.prototype.hasOwnProperty.call(value, i)) {
      return formatSpecialArray(ctx, value, recurseTimes, len, output, i);
    }
    output.push(formatProperty(ctx, value, recurseTimes, i, kArrayType));
  }
  if (remaining > 0) {
    output.push(remainingText(remaining));
  }
  return output;
}

function formatTypedArray(
  value: NodeJS.TypedArray,
  length: number,
  ctx: Context,
  _ignored: unknown,
  recurseTimes: number
): string[] {
  const maxLength = Math.min(Math.max(0, ctx.maxArrayLength), length);
  const remaining = value.length - maxLength;
  const output = new Array<string>(maxLength);
  const elementFormatter =
    value.length > 0 && typeof value[0] === 'number'
      ? formatNumber
      : formatBigInt;
  for (let i = 0; i < maxLength; ++i) {
    // @ts-expect-error `value[i]` assumed to be of correct numeric type
    output[i] = elementFormatter(ctx.stylize, value[i], ctx.numericSeparator);
  }
  if (remaining > 0) {
    output[maxLength] = remainingText(remaining);
  }
  if (ctx.showHidden) {
    // .buffer goes last, it's not a primitive like the others.
    // All besides `BYTES_PER_ELEMENT` are actually getters.
    ctx.indentationLvl += 2;
    for (const key of [
      'BYTES_PER_ELEMENT',
      'length',
      'byteLength',
      'byteOffset',
      'buffer',
    ] as const) {
      const str = formatValue(ctx, value[key], recurseTimes, true);
      output.push(`[${key}]: ${str}`);
    }
    ctx.indentationLvl -= 2;
  }
  return output;
}

function formatSet(
  value: Set<unknown> | IterableIterator<unknown>,
  ctx: Context,
  _ignored: unknown,
  recurseTimes: number
): string[] {
  const length = isSet(value) ? value.size : NaN;
  const maxLength = Math.min(Math.max(0, ctx.maxArrayLength), length);
  const remaining = length - maxLength;
  const output: string[] = [];
  ctx.indentationLvl += 2;
  let i = 0;
  for (const v of value) {
    if (i >= maxLength) break;
    output.push(formatValue(ctx, v, recurseTimes));
    i++;
  }
  if (remaining > 0) {
    output.push(remainingText(remaining));
  }
  ctx.indentationLvl -= 2;
  return output;
}

function formatMap(
  value: Map<unknown, unknown> | IterableIterator<[unknown, unknown]>,
  ctx: Context,
  _ignored: unknown,
  recurseTimes: number
): string[] {
  const length = isMap(value) ? value.size : NaN;
  const maxLength = Math.min(Math.max(0, ctx.maxArrayLength), length);
  const remaining = length - maxLength;
  const output: string[] = [];
  ctx.indentationLvl += 2;
  let i = 0;
  for (const { 0: k, 1: v } of value) {
    if (i >= maxLength) break;
    output.push(
      `${formatValue(ctx, k, recurseTimes)} => ${formatValue(ctx, v, recurseTimes)}`
    );
    i++;
  }
  if (remaining > 0) {
    output.push(remainingText(remaining));
  }
  ctx.indentationLvl -= 2;
  return output;
}

function formatSetIterInner(
  ctx: Context,
  recurseTimes: number,
  entries: unknown[],
  state: number
): string[] {
  const maxArrayLength = Math.max(ctx.maxArrayLength, 0);
  const maxLength = Math.min(maxArrayLength, entries.length);
  const output = new Array<string>(maxLength);
  ctx.indentationLvl += 2;
  for (let i = 0; i < maxLength; i++) {
    output[i] = formatValue(ctx, entries[i], recurseTimes);
  }
  ctx.indentationLvl -= 2;
  if (state === kWeak && !ctx.sorted) {
    // Sort all entries to have a halfway reliable output (if more entries than
    // retrieved ones exist, we can not reliably return the same output) if the
    // output is not sorted anyway.
    output.sort();
  }
  const remaining = entries.length - maxLength;
  if (remaining > 0) {
    output.push(remainingText(remaining));
  }
  return output;
}

function formatMapIterInner(
  ctx: Context,
  recurseTimes: number,
  entries: unknown[],
  state: number
): string[] {
  const maxArrayLength = Math.max(ctx.maxArrayLength, 0);
  // Entries exist as [key1, val1, key2, val2, ...]
  const len = entries.length / 2;
  const remaining = len - maxArrayLength;
  const maxLength = Math.min(maxArrayLength, len);
  const output = new Array<string>(maxLength);
  let i = 0;
  ctx.indentationLvl += 2;
  if (state === kWeak) {
    for (; i < maxLength; i++) {
      const pos = i * 2;
      output[i] =
        `${formatValue(ctx, entries[pos], recurseTimes)} => ${formatValue(ctx, entries[pos + 1], recurseTimes)}`;
    }
    // Sort all entries to have a halfway reliable output (if more entries than
    // retrieved ones exist, we can not reliably return the same output) if the
    // output is not sorted anyway.
    if (!ctx.sorted) output.sort();
  } else {
    for (; i < maxLength; i++) {
      const pos = i * 2;
      const res = [
        formatValue(ctx, entries[pos], recurseTimes),
        formatValue(ctx, entries[pos + 1], recurseTimes),
      ];
      output[i] = reduceToSingleString(
        ctx,
        res,
        '',
        ['[', ']'],
        kArrayExtrasType,
        recurseTimes
      );
    }
  }
  ctx.indentationLvl -= 2;
  if (remaining > 0) {
    output.push(remainingText(remaining));
  }
  return output;
}

function formatWeakCollection(ctx: Context): string[] {
  return [ctx.stylize('<items unknown>', 'special')];
}

function formatWeakSet(
  ctx: Context,
  value: WeakSet<any>,
  recurseTimes: number
): string[] {
  const { entries } = internal.previewEntries(value)!;
  return formatSetIterInner(ctx, recurseTimes, entries, kWeak);
}

function formatWeakMap(
  ctx: Context,
  value: WeakMap<any, unknown>,
  recurseTimes: number
): string[] {
  const { entries } = internal.previewEntries(value)!;
  return formatMapIterInner(ctx, recurseTimes, entries, kWeak);
}

function formatIterator(
  braces: [string, string],
  ctx: Context,
  value: Iterator<unknown>,
  recurseTimes: number
): string[] {
  const { entries, isKeyValue } = internal.previewEntries(value)!;
  if (isKeyValue) {
    // Mark entry iterators as such.
    braces[0] = braces[0].replace(/ Iterator] {$/, ' Entries] {');
    return formatMapIterInner(ctx, recurseTimes, entries, kMapEntries);
  }

  return formatSetIterInner(ctx, recurseTimes, entries, kIterator);
}

function formatPromise(
  ctx: Context,
  value: Promise<unknown>,
  recurseTimes: number
): string[] {
  let output: string[];
  const { state, result } = internal.getPromiseDetails(value)!;
  if (state === internal.kPending) {
    output = [ctx.stylize('<pending>', 'special')];
  } else {
    ctx.indentationLvl += 2;
    const str = formatValue(ctx, result, recurseTimes);
    ctx.indentationLvl -= 2;
    output = [
      state === internal.kRejected
        ? `${ctx.stylize('<rejected>', 'special')} ${str}`
        : str,
    ];
  }
  return output;
}

function formatProperty(
  ctx: Context,
  value: object,
  recurseTimes: number,
  key: PropertyKey,
  type: number,
  desc?: PropertyDescriptor,
  original = value
): string {
  let name: string, str: string;
  let extra = ' ';
  desc = desc ||
    Object.getOwnPropertyDescriptor(value, key) || {
      value: (value as Record<PropertyKey, unknown>)[key],
      enumerable: true,
    };
  if (desc.value !== undefined) {
    const diff = ctx.compact !== true || type !== kObjectType ? 2 : 3;
    ctx.indentationLvl += diff;
    str = formatValue(ctx, desc.value, recurseTimes);
    if (diff === 3 && ctx.breakLength < getStringWidth(str, ctx.colors)) {
      extra = `\n${' '.repeat(ctx.indentationLvl)}`;
    }
    ctx.indentationLvl -= diff;
  } else if (desc.get !== undefined) {
    const label = desc.set !== undefined ? 'Getter/Setter' : 'Getter';
    const s = ctx.stylize;
    const sp = 'special';
    if (
      ctx.getters &&
      (ctx.getters === true ||
        (ctx.getters === 'get' && desc.set === undefined) ||
        (ctx.getters === 'set' && desc.set !== undefined))
    ) {
      try {
        const tmp = desc.get.call(original);
        ctx.indentationLvl += 2;
        if (tmp === null) {
          str = `${s(`[${label}:`, sp)} ${s('null', 'null')}${s(']', sp)}`;
        } else if (typeof tmp === 'object') {
          str = `${s(`[${label}]`, sp)} ${formatValue(ctx, tmp, recurseTimes)}`;
        } else {
          const primitive = formatPrimitive(s, tmp, ctx);
          str = `${s(`[${label}:`, sp)} ${primitive}${s(']', sp)}`;
        }
        ctx.indentationLvl -= 2;
      } catch (err) {
        const message = `<Inspection threw (${isError(err) ? err.message : String(err)})>`;
        str = `${s(`[${label}:`, sp)} ${message}${s(']', sp)}`;
      }
    } else {
      str = ctx.stylize(`[${label}]`, sp);
    }
  } else if (desc.set !== undefined) {
    str = ctx.stylize('[Setter]', 'special');
  } else {
    str = ctx.stylize('undefined', 'undefined');
  }
  if (type === kArrayType) {
    return str;
  }
  if (typeof key === 'symbol') {
    const tmp = Symbol.prototype.toString
      .call(key)
      .replace(strEscapeSequencesReplacer, escapeFn);
    name = ctx.stylize(tmp, 'symbol');
  } else if (keyStrRegExp.exec(key as string) !== null) {
    name =
      key === '__proto__'
        ? "['__proto__']"
        : ctx.stylize(key as string, 'name');
  } else {
    name = ctx.stylize(strEscape(key as string), 'string');
  }
  if (desc.enumerable === false) {
    name = `[${name}]`;
  }
  return `${name}:${extra}${str}`;
}

function isBelowBreakLength(
  ctx: Context,
  output: string[],
  start: number,
  base: string
): boolean {
  // Each entry is separated by at least a comma. Thus, we start with a total
  // length of at least `output.length`. In addition, some cases have a
  // whitespace in-between each other that is added to the total as well.
  // TODO(BridgeAR): Add Unicode support. Use the readline getStringWidth
  // function. Check the performance overhead and make it an opt-in in case it's
  // significant.
  let totalLength = output.length + start;
  if (totalLength + output.length > ctx.breakLength) return false;
  for (let i = 0; i < output.length; i++) {
    if (ctx.colors) {
      totalLength += removeColors(output[i]!).length;
    } else {
      totalLength += output[i]!.length;
    }
    if (totalLength > ctx.breakLength) {
      return false;
    }
  }
  // Do not line up properties on the same line if `base` contains line breaks.
  return base === '' || !base.includes('\n');
}

function reduceToSingleString(
  ctx: Context,
  output: string[],
  base: string,
  braces: [string, string],
  extrasType: number,
  recurseTimes: number,
  value?: unknown
): string {
  if (ctx.compact !== true) {
    if (typeof ctx.compact === 'number' && ctx.compact >= 1) {
      // Memorize the original output length. In case the output is grouped,
      // prevent lining up the entries on a single line.
      const entries = output.length;
      // Group array elements together if the array contains at least six
      // separate entries.
      if (extrasType === kArrayExtrasType && entries > 6) {
        output = groupArrayElements(ctx, output, value as unknown[]);
      }
      // `ctx.currentDepth` is set to the most inner depth of the currently
      // inspected object part while `recurseTimes` is the actual current depth
      // that is inspected.
      //
      // Example:
      //
      // const a = { first: [ 1, 2, 3 ], second: { inner: [ 1, 2, 3 ] } }
      //
      // The deepest depth of `a` is 2 (a.second.inner) and `a.first` has a max
      // depth of 1.
      //
      // Consolidate all entries of the local most inner depth up to
      // `ctx.compact`, as long as the properties are smaller than
      // `ctx.breakLength`.
      if (
        ctx.currentDepth - recurseTimes < ctx.compact &&
        entries === output.length
      ) {
        // Line up all entries on a single line in case the entries do not
        // exceed `breakLength`. Add 10 as constant to start next to all other
        // factors that may reduce `breakLength`.
        const start =
          output.length +
          ctx.indentationLvl +
          braces[0].length +
          base.length +
          10;
        if (isBelowBreakLength(ctx, output, start, base)) {
          const joinedOutput = output.join(', ');
          if (!joinedOutput.includes('\n')) {
            return (
              `${base ? `${base} ` : ''}${braces[0]} ${joinedOutput}` +
              ` ${braces[1]}`
            );
          }
        }
      }
    }
    // Line up each entry on an individual line.
    const indentation = `\n${' '.repeat(ctx.indentationLvl)}`;
    return (
      `${base ? `${base} ` : ''}${braces[0]}${indentation}  ` +
      `${output.join(`,${indentation}  `)}${indentation}${braces[1]}`
    );
  }
  // Line up all entries on a single line in case the entries do not exceed
  // `breakLength`.
  if (isBelowBreakLength(ctx, output, 0, base)) {
    return (
      `${braces[0]}${base ? ` ${base}` : ''} ${output.join(', ')} ` + braces[1]
    );
  }
  const indentation = ' '.repeat(ctx.indentationLvl);
  // If the opening "brace" is too large, like in the case of "Set {",
  // we need to force the first item to be on the next line or the
  // items will not line up correctly.
  const ln =
    base === '' && braces[0].length === 1
      ? ' '
      : `${base ? ` ${base}` : ''}\n${indentation}  `;
  // Line up each entry on an individual line.
  return `${braces[0]}${ln}${output.join(`,\n${indentation}  `)} ${braces[1]}`;
}

function hasBuiltInToString(value: object): boolean {
  // Prevent triggering proxy traps.
  const proxyTarget = internal.getProxyDetails(value);
  if (proxyTarget !== undefined) {
    if (proxyTarget === null || proxyTarget.target === null) {
      return true;
    }
    return hasBuiltInToString(proxyTarget.target as object);
  }

  // Count objects that have no `toString` function as built-in.
  if (typeof value?.toString !== 'function') {
    return true;
  }

  // The object has a own `toString` property. Thus it's not not a built-in one.
  if (Object.prototype.hasOwnProperty.call(value, 'toString')) {
    return false;
  }

  // Find the object that has the `toString` property as own property in the
  // prototype chain.
  let pointer = value;
  do {
    pointer = Object.getPrototypeOf(pointer);
  } while (!Object.prototype.hasOwnProperty.call(pointer, 'toString'));

  // Check closer if the object is a built-in.
  const descriptor = Object.getOwnPropertyDescriptor(pointer, 'constructor');
  return (
    descriptor !== undefined &&
    typeof descriptor.value === 'function' &&
    builtInObjects.has(descriptor.value.name)
  );
}

const firstErrorLine = (error: unknown) =>
  (isError(error) ? error.message : String(error)).split('\n', 1)[0];
let CIRCULAR_ERROR_MESSAGE: string | undefined;
function tryStringify(arg: unknown): string {
  try {
    return JSON.stringify(arg);
  } catch (err) {
    // Populate the circular error message lazily
    if (!CIRCULAR_ERROR_MESSAGE) {
      try {
        const a: { a?: unknown } = {};
        a.a = a;
        JSON.stringify(a);
      } catch (circularError) {
        CIRCULAR_ERROR_MESSAGE = firstErrorLine(circularError);
      }
    }
    if (
      typeof err === 'object' &&
      err !== null &&
      'name' in err &&
      err.name === 'TypeError' &&
      firstErrorLine(err) === CIRCULAR_ERROR_MESSAGE
    ) {
      return '[Circular]';
    }
    throw err;
  }
}

export function format(...args: unknown[]): string {
  return formatWithOptionsInternal(undefined, args);
}

export function formatWithOptions(
  inspectOptions: InspectOptions,
  ...args: unknown[]
): string {
  validateObject(inspectOptions, 'inspectOptions', kValidateObjectAllowArray);
  return formatWithOptionsInternal(inspectOptions, args);
}

function formatNumberNoColor(number: number, options?: InspectOptions): string {
  return formatNumber(
    stylizeNoColor,
    number,
    options?.numericSeparator ?? inspectDefaultOptions.numericSeparator
  );
}

function formatBigIntNoColor(bigint: bigint, options?: InspectOptions): string {
  return formatBigInt(
    stylizeNoColor,
    bigint,
    options?.numericSeparator ?? inspectDefaultOptions.numericSeparator
  );
}

function formatWithOptionsInternal(
  inspectOptions: InspectOptions | undefined,
  args: unknown[]
): string {
  const first = args[0];
  let a = 0;
  let str = '';
  let join = '';

  if (typeof first === 'string') {
    if (args.length === 1) {
      return first;
    }
    let tempStr;
    let lastPos = 0;

    for (let i = 0; i < first.length - 1; i++) {
      if (first.charCodeAt(i) === 37) {
        // '%'
        const nextChar = first.charCodeAt(++i);
        if (a + 1 !== args.length) {
          switch (nextChar) {
            case 115: {
              // 's'
              const tempArg = args[++a];
              if (typeof tempArg === 'number') {
                tempStr = formatNumberNoColor(tempArg, inspectOptions);
              } else if (typeof tempArg === 'bigint') {
                tempStr = formatBigIntNoColor(tempArg, inspectOptions);
              } else if (
                typeof tempArg !== 'object' ||
                tempArg === null ||
                !hasBuiltInToString(tempArg)
              ) {
                tempStr = String(tempArg);
              } else {
                tempStr = inspect(tempArg, {
                  ...inspectOptions,
                  compact: 3,
                  colors: false,
                  depth: 0,
                });
              }
              break;
            }
            case 106: // 'j'
              tempStr = tryStringify(args[++a]);
              break;
            case 100: {
              // 'd'
              const tempNum = args[++a];
              if (typeof tempNum === 'bigint') {
                tempStr = formatBigIntNoColor(tempNum, inspectOptions);
              } else if (typeof tempNum === 'symbol') {
                tempStr = 'NaN';
              } else {
                tempStr = formatNumberNoColor(Number(tempNum), inspectOptions);
              }
              break;
            }
            case 79: // 'O'
              tempStr = inspect(args[++a], inspectOptions);
              break;
            case 111: // 'o'
              tempStr = inspect(args[++a], {
                ...inspectOptions,
                showHidden: true,
                showProxy: true,
                depth: 4,
              });
              break;
            case 105: {
              // 'i'
              const tempInteger = args[++a];
              if (typeof tempInteger === 'bigint') {
                tempStr = formatBigIntNoColor(tempInteger, inspectOptions);
              } else if (typeof tempInteger === 'symbol') {
                tempStr = 'NaN';
              } else {
                tempStr = formatNumberNoColor(
                  Number.parseInt(tempInteger as unknown as string),
                  inspectOptions
                );
              }
              break;
            }
            case 102: {
              // 'f'
              const tempFloat = args[++a];
              if (typeof tempFloat === 'symbol') {
                tempStr = 'NaN';
              } else {
                tempStr = formatNumberNoColor(
                  Number.parseFloat(tempFloat as unknown as string),
                  inspectOptions
                );
              }
              break;
            }
            case 99: // 'c'
              a += 1;
              tempStr = '';
              break;
            case 37: // '%'
              str += first.slice(lastPos, i);
              lastPos = i + 1;
              continue;
            default: // Any other character is not a correct placeholder
              continue;
          }
          if (lastPos !== i - 1) {
            str += first.slice(lastPos, i - 1);
          }
          str += tempStr;
          lastPos = i + 1;
        } else if (nextChar === 37) {
          str += first.slice(lastPos, i);
          lastPos = i + 1;
        }
      }
    }
    if (lastPos !== 0) {
      a++;
      join = ' ';
      if (lastPos < first.length) {
        str += first.slice(lastPos);
      }
    }
  }

  while (a < args.length) {
    const value = args[a];
    str += join;
    str += typeof value !== 'string' ? inspect(value, inspectOptions) : value;
    join = ' ';
    a++;
  }
  return str;
}

export function isZeroWidthCodePoint(code: number): boolean {
  return (
    code <= 0x1f || // C0 control codes
    (code >= 0x7f && code <= 0x9f) || // C1 control codes
    (code >= 0x300 && code <= 0x36f) || // Combining Diacritical Marks
    (code >= 0x200b && code <= 0x200f) || // Modifying Invisible Characters
    // Combining Diacritical Marks for Symbols
    (code >= 0x20d0 && code <= 0x20ff) ||
    (code >= 0xfe00 && code <= 0xfe0f) || // Variation Selectors
    (code >= 0xfe20 && code <= 0xfe2f) || // Combining Half Marks
    (code >= 0xe0100 && code <= 0xe01ef)
  ); // Variation Selectors
}

/**
 * Returns the number of columns required to display the given string.
 */
export function getStringWidth(str: string, removeControlChars = true): number {
  let width = 0;

  if (removeControlChars) str = stripVTControlCharacters(str);
  str = str.normalize('NFC');
  for (const char of str) {
    const code = char.codePointAt(0)!;
    if (isFullWidthCodePoint(code)) {
      width += 2;
    } else if (!isZeroWidthCodePoint(code)) {
      width++;
    }
  }

  return width;
}

/**
 * Returns true if the character represented by a given
 * Unicode code point is full-width. Otherwise returns false.
 */
const isFullWidthCodePoint = (code: number) => {
  // Code points are partially derived from:
  // https://www.unicode.org/Public/UNIDATA/EastAsianWidth.txt
  return (
    code >= 0x1100 &&
    (code <= 0x115f || // Hangul Jamo
      code === 0x2329 || // LEFT-POINTING ANGLE BRACKET
      code === 0x232a || // RIGHT-POINTING ANGLE BRACKET
      // CJK Radicals Supplement .. Enclosed CJK Letters and Months
      (code >= 0x2e80 && code <= 0x3247 && code !== 0x303f) ||
      // Enclosed CJK Letters and Months .. CJK Unified Ideographs Extension A
      (code >= 0x3250 && code <= 0x4dbf) ||
      // CJK Unified Ideographs .. Yi Radicals
      (code >= 0x4e00 && code <= 0xa4c6) ||
      // Hangul Jamo Extended-A
      (code >= 0xa960 && code <= 0xa97c) ||
      // Hangul Syllables
      (code >= 0xac00 && code <= 0xd7a3) ||
      // CJK Compatibility Ideographs
      (code >= 0xf900 && code <= 0xfaff) ||
      // Vertical Forms
      (code >= 0xfe10 && code <= 0xfe19) ||
      // CJK Compatibility Forms .. Small Form Variants
      (code >= 0xfe30 && code <= 0xfe6b) ||
      // Halfwidth and Fullwidth Forms
      (code >= 0xff01 && code <= 0xff60) ||
      (code >= 0xffe0 && code <= 0xffe6) ||
      // Kana Supplement
      (code >= 0x1b000 && code <= 0x1b001) ||
      // Enclosed Ideographic Supplement
      (code >= 0x1f200 && code <= 0x1f251) ||
      // Miscellaneous Symbols and Pictographs 0x1f300 - 0x1f5ff
      // Emoticons 0x1f600 - 0x1f64f
      (code >= 0x1f300 && code <= 0x1f64f) ||
      // CJK Unified Ideographs Extension B .. Tertiary Ideographic Plane
      (code >= 0x20000 && code <= 0x3fffd))
  );
};

/**
 * Remove all VT control characters. Use to estimate displayed string width.
 */
export function stripVTControlCharacters(str: string): string {
  validateString(str, 'str');

  return str.replace(ansi, '');
}

// ================================================================================================
// WORKERD SPECIFIC CODE

// Called from C++ on `console.log()`s to format values
export function formatLog(
  ...args: [
    ...values: unknown[],
    colors: boolean,
    structuredLogging: boolean,
    level: string,
  ]
): string {
  const level = args.pop() as string;
  const structuredLogging = args.pop() as boolean;
  const colors = args.pop() as boolean;
  const inspectOptions: InspectOptions = { colors };

  try {
    const message = formatWithOptions(inspectOptions, ...args);
    if (structuredLogging) {
      return JSON.stringify({
        timestamp: Date.now(),
        level,
        message,
      });
    } else {
      return message;
    }
  } catch (err) {
    return `<Formatting threw (${isError(err) ? err.stack : String(err)})>`;
  }
}

function isBuiltinPrototype(proto: unknown) {
  if (proto === null) return true;
  const descriptor = Object.getOwnPropertyDescriptor(proto, 'constructor');
  return (
    descriptor !== undefined &&
    typeof descriptor.value === 'function' &&
    builtInObjects.has(descriptor.value.name)
  );
}

function isRpcWildcardType(value: unknown) {
  return (
    value instanceof internalWorkers.RpcStub ||
    value instanceof internalWorkers.RpcPromise ||
    value instanceof internalWorkers.RpcProperty
  );
}

function isEntry(value: unknown): value is [unknown, unknown] {
  return Array.isArray(value) && value.length === 2;
}
function maybeGetEntries(
  value: Record<PropertyKey, unknown>
): [unknown, unknown][] | undefined {
  // If this value is an RPC type with a wildcard property handler (e.g. `RpcStub`), don't try to
  // call `entries()` on it. This won't be an `entries()` function, and calling it with `.call()`
  // would dispose the stub.
  if (isRpcWildcardType(value)) return;

  const entriesFunction = value['entries'] as any;
  if (typeof entriesFunction !== 'function') return;
  const entriesIterator: unknown = entriesFunction.call(value);
  if (typeof entriesIterator !== 'object' || entriesIterator === null) return;
  if (!(Symbol.iterator in entriesIterator)) return;
  const entries = Array.from(entriesIterator as Iterable<unknown>);
  if (!entries.every(isEntry)) return;
  return entries;
}

const kEntries = Symbol('kEntries');
function hasEntries(
  value: unknown
): value is { [kEntries]: [unknown, unknown][] } {
  return typeof value === 'object' && value !== null && kEntries in value;
}

// Default custom inspect implementation for JSG resource types
function formatJsgResourceType(
  this: Record<PropertyKey, unknown>,
  additionalProperties: Record<
    string,
    symbol /* value-func */ | false /* unimplemented-marker */
  >,
  depth: number,
  options: InspectOptionsStylized
): unknown {
  const name = this.constructor.name;
  if (depth < 0) return options.stylize(`[${name}]`, 'special');

  // Build a plain object for inspection. If this value has an `entries()` function, add those
  // entries for map-like `K => V` formatting. Note we can't use a `Map` here as a key may have
  // multiple values (e.g. URLSearchParams).
  const record: Record<PropertyKey, unknown> = {};
  const maybeEntries = maybeGetEntries(this);
  if (maybeEntries !== undefined) record[kEntries] = maybeEntries;

  // Add all instance and prototype non-function-valued properties
  let current: object = this;
  do {
    // `Object.getOwnPropertyDescriptor()` throws `Illegal Invocation` for our prototypes here.
    for (const key of Object.getOwnPropertyNames(current)) {
      // If this property is unimplemented, don't try to log it
      if (additionalProperties[key] === false) continue;
      const value = this[key];
      // Ignore function-valued and static properties
      if (
        typeof value === 'function' ||
        this.constructor.propertyIsEnumerable(key)
      )
        continue;
      record[key] = value;
    }
  } while (!isBuiltinPrototype((current = Object.getPrototypeOf(current))));

  // Add additional inspect-only properties as non-enumerable so they appear in square brackets
  for (const [key, symbol] of Object.entries(additionalProperties)) {
    // This is an additional property if it's not an unimplemented marker
    if (symbol !== false) {
      Object.defineProperty(record, key, {
        value: this[symbol],
        enumerable: false,
      });
    }
  }

  // Format the plain object
  const inspected = inspect(record, {
    ...options,
    depth: options.depth == null ? null : depth,
    showHidden: true, // Show non-enumerable inspect-only properties
  });

  if (maybeEntries === undefined) {
    return `${name} ${inspected}`;
  } else {
    // Inspecting a entries object gives something like `Object(1) { 'a' => '1' }`, whereas we want
    // something like `Headers(1) { 'a' => '1' }`.
    return `${name}${inspected.replace('Object', '')}`;
  }
}
