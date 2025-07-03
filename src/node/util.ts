// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/* TODO: the following is adopted code, enabling linting one day */
/* eslint-disable */

import { default as internalTypes } from 'node-internal:internal_types';
import { default as utilImpl } from 'node-internal:util';
import { isDeepStrictEqual as _isDeepStrictEqual } from 'node-internal:internal_comparisons';

import {
  validateFunction,
  validateAbortSignal,
  validateObject,
  kValidateObjectAllowObjects,
  validateString,
  validateBoolean,
  validateOneOf,
} from 'node-internal:validators';

import { debuglog } from 'node-internal:debuglog';
export const debug = debuglog;
export { debuglog };

import { ERR_INVALID_ARG_TYPE } from 'node-internal:internal_errors';

import {
  inspect,
  format,
  formatWithOptions,
  stripVTControlCharacters,
} from 'node-internal:internal_inspect';

import { callbackify, parseEnv } from 'node-internal:internal_utils';
import {
  isReadableStream,
  isWritableStream,
  isNodeStream,
} from 'node-internal:streams_util';
export { inspect, format, formatWithOptions, stripVTControlCharacters };
export { callbackify, parseEnv } from 'node-internal:internal_utils';
export const types = internalTypes;

export const { MIMEParams, MIMEType } = utilImpl;

const kCustomPromisifiedSymbol = Symbol.for('nodejs.util.promisify.custom');
const kCustomPromisifyArgsSymbol = Symbol.for(
  'nodejs.util.promisify.custom.args'
);

// TODO(later): Proper type signature for promisify.
export function promisify(original: Function): Function {
  validateFunction(original, 'original');

  if ((original as any)[kCustomPromisifiedSymbol]) {
    const fn = (original as any)[kCustomPromisifiedSymbol];

    validateFunction(fn, 'util.promisify.custom');

    return Object.defineProperty(fn, kCustomPromisifiedSymbol, {
      value: fn,
      enumerable: false,
      writable: false,
      configurable: true,
    });
  }

  // Names to create an object from in case the callback receives multiple
  // arguments, e.g. ['bytesRead', 'buffer'] for fs.read.
  const argumentNames = (original as any)[kCustomPromisifyArgsSymbol];

  function fn(this: any, ...args: any[]) {
    return new Promise((resolve, reject) => {
      args.push((err: unknown, ...values: unknown[]) => {
        if (err) {
          return reject(err);
        }
        if (argumentNames !== undefined && values.length > 1) {
          const obj = {};
          for (let i = 0; i < argumentNames.length; i++)
            (obj as any)[argumentNames[i]] = values[i];
          resolve(obj);
        } else {
          resolve(values[0]);
        }
      });
      Reflect.apply(original, this, args);
    });
  }

  Object.setPrototypeOf(fn, Object.getPrototypeOf(original));

  Object.defineProperty(fn, kCustomPromisifiedSymbol, {
    value: fn,
    enumerable: false,
    writable: false,
    configurable: true,
  });

  const descriptors = Object.getOwnPropertyDescriptors(original);
  const propertiesValues = Object.values(descriptors);
  for (let i = 0; i < propertiesValues.length; i++) {
    // We want to use null-prototype objects to not rely on globally mutable
    // %Object.prototype%.
    Object.setPrototypeOf(propertiesValues[i], null);
  }
  return Object.defineProperties(fn, descriptors);
}

promisify.custom = kCustomPromisifiedSymbol;

export function inherits(ctor: Function, superCtor: Function) {
  if (ctor === undefined || ctor === null)
    throw new ERR_INVALID_ARG_TYPE('ctor', 'Function', ctor);

  if (superCtor === undefined || superCtor === null)
    throw new ERR_INVALID_ARG_TYPE('superCtor', 'Function', superCtor);

  if (superCtor.prototype === undefined) {
    throw new ERR_INVALID_ARG_TYPE(
      'superCtor.prototype',
      'Object',
      superCtor.prototype
    );
  }
  Object.defineProperty(ctor, 'super_', {
    value: superCtor,
    writable: true,
    configurable: true,
  });
  Object.setPrototypeOf(ctor.prototype, superCtor.prototype);
}

export function _extend(target: Object, source: Object) {
  // Don't do anything if source isn't an object
  if (source === null || typeof source !== 'object') return target;

  const keys = Object.keys(source);
  let i = keys.length;
  while (i--) {
    (target as any)[keys[i]!] = (source as any)[keys[i]!];
  }
  return target;
}

export const TextDecoder = globalThis.TextDecoder;
export const TextEncoder = globalThis.TextEncoder;

export function toUSVString(input: any) {
  return input.toWellFormed();
}

function pad(n: any): string {
  return `${n}`.padStart(2, '0');
}

// prettier-ignore
const months = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep',
                'Oct', 'Nov', 'Dec'];

function timestamp(): string {
  const d = new Date();
  const t = [pad(d.getHours()), pad(d.getMinutes()), pad(d.getSeconds())].join(
    ':'
  );
  return `${d.getDate()} ${months[d.getMonth()]} ${t}`;
}

export function log(...args: any[]) {
  console.log('%s - %s', timestamp(), format(...args));
}

export function parseArgs(..._: any[]): any {
  // We currently have no plans to implement the util.parseArgs API.
  throw new Error('node:util parseArgs is not implemented');
}

export function transferableAbortController(..._: any[]): any {
  throw new Error('node:util transferableAbortController is not implemented');
}

export function transferableAbortSignal(..._: any[]): any {
  throw new Error('node:util transferableAbortSignal is not implemented');
}

export async function aborted(signal: AbortSignal, resource: object) {
  if (signal === undefined) {
    throw new ERR_INVALID_ARG_TYPE('signal', 'AbortSignal', signal);
  }
  // Node.js defines that the resource is held weakly such that if it is GC'ed, we
  // will drop the event handler on the signal and the promise will remain pending
  // forever. We don't want GC to be observable in the same way so we won't support
  // this additional option. Unfortunately Node.js does not make this argument optional.
  // We'll just ignore it.
  validateAbortSignal(signal, 'signal');
  validateObject(resource, 'resource', kValidateObjectAllowObjects);
  if (signal.aborted) return Promise.resolve();
  const { promise, resolve } = Promise.withResolvers();
  const opts = { __proto__: null, once: true };
  signal.addEventListener('abort', resolve, opts);
  return promise;
}

export function deprecate(
  fn: Function,
  _1?: string,
  _2?: string,
  _3?: boolean
) {
  // TODO(soon): Node.js's implementation wraps the given function in a new function that
  // logs a warning to the console if the function is called. Do we want to support that?
  // For now, we're just going to silently return the input method unmodified.
  return fn;
}

// Node.js originally introduced the API with the name `getCallSite()` as an experimental
// API but then renamed it to `getCallSites()` soon after. We had already implemented the
// API with the original name in a release. To avoid the possibility of breaking, we export
// the function using both names.
export const getCallSite = utilImpl.getCallSites.bind(utilImpl);
export const getCallSites = utilImpl.getCallSites.bind(utilImpl);

export function isDeepStrictEqual(a: unknown, b: unknown): boolean {
  return _isDeepStrictEqual(a, b);
}

export function isArray(a: unknown): a is Array<any> {
  return Array.isArray(a);
}

export function getSystemErrorMap(): void {
  throw new Error('node:util getSystemErrorMap is not implemented');
}

export function getSystemErrorName(): void {
  throw new Error('node:util getSystemErrorName is not implemented');
}

export function getSystemErrorMessage(): void {
  throw new Error('node:util getSystemErrorMessage is not implemented');
}

function escapeStyleCode(code: number | undefined): string {
  if (code === undefined) return '';
  return `\u001b[${code}m`;
}

// TODO(soon): We do not yet implement process.stdout, so to ensure the correct
// behaviour, we use a placeholder value internally.

interface StyleTextOptions {
  validateStream?: boolean;
  stream?: ReadableStream | WritableStream;
}

type TTYStream = (ReadableStream | WritableStream) & {
  isTTY: true;
  getColorDepth(): number;
};

function isTTYStream(
  stream: ReadableStream | WritableStream
): stream is TTYStream {
  return (
    typeof stream === 'object' &&
    'isTTY' in stream &&
    !!stream.isTTY &&
    typeof (stream as TTYStream).getColorDepth === 'function'
  );
}

const stdoutPlaceholder = Object.create(null);
export function styleText(
  format: string | string[],
  text: string,
  { validateStream = true, stream = stdoutPlaceholder }: StyleTextOptions = {}
): string {
  validateString(text, 'text');
  if (validateStream !== true)
    validateBoolean(validateStream, 'options.validateStream');

  let skipColorize: boolean = false;
  if (validateStream) {
    if (
      !isReadableStream(stream) &&
      !isWritableStream(stream) &&
      !isNodeStream(stream) &&
      stream !== stdoutPlaceholder
    ) {
      throw new ERR_INVALID_ARG_TYPE(
        'stream',
        ['ReadableStream', 'WritableStream', 'Stream'],
        stream
      );
    }

    // If the stream is falsy or should not be colorized, set skipColorize to true
    skipColorize = isTTYStream(stream) ? stream.getColorDepth() > 2 : true;
  }

  // If the format is not an array, convert it to an array
  const formatArray = Array.isArray(format) ? format : [format];

  let left = '';
  let right = '';
  for (const key of formatArray) {
    if (key === 'none') continue;
    const formatCodes =
      typeof key === 'string' ? inspect.colors[key] : undefined;
    // If the format is not a valid style, throw an error
    if (formatCodes == null) {
      validateOneOf(key, 'format', Object.keys(inspect.colors));
    }
    if (skipColorize) continue;
    left += escapeStyleCode(formatCodes ? formatCodes[0] : undefined);
    right = `${escapeStyleCode(formatCodes ? formatCodes[1] : undefined)}${right}`;
  }

  return skipColorize ? text : `${left}${text}${right}`;
}

export default {
  types,
  callbackify,
  promisify,
  inspect,
  format,
  formatWithOptions,
  stripVTControlCharacters,
  inherits,
  _extend,
  MIMEParams,
  MIMEType,
  toUSVString,
  log,
  aborted,
  debuglog,
  debug,
  deprecate,
  getSystemErrorMap,
  getSystemErrorMessage,
  getSystemErrorName,
  // Node.js originally exposed TextEncoder and TextDecoder off the util
  // module originally, so let's just go ahead and do the same.
  TextEncoder,
  TextDecoder,
  parseArgs,
  parseEnv,
  styleText,
  transferableAbortController,
  transferableAbortSignal,
  getCallSite,
  getCallSites,
  isDeepStrictEqual,
  isArray,
};
