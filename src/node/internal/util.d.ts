// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/* eslint-disable @typescript-eslint/no-explicit-any */
/* eslint-disable @typescript-eslint/ban-types */

export abstract class MIMEType {
  public constructor(input: string);
  public type: string;
  public subtype: string;
  public readonly essence: string;
  public readonly params: MIMEParams;
  public toString(): string;
  public toJSON(): string;
}

export abstract class MIMEParams {
  public constructor();
  public delete(name: string): void;
  public get(name: string): string|undefined;
  public has(name: string): boolean;
  public set(name: string, value: string): void;
  public entries(): Iterable<string[]>;
  public keys(): Iterable<string>;
  public values(): Iterable<string>;
}

export const kResourceTypeInspect: unique symbol;

export const ALL_PROPERTIES: 0;
export const ONLY_ENUMERABLE: 1;
export function getOwnNonIndexProperties(value: unknown, filter: typeof ALL_PROPERTIES | typeof ONLY_ENUMERABLE): PropertyKey[];

export const kPending: 0;
export const kFulfilled: 1;
export const kRejected: 2;
export interface PromiseDetails {
  state: typeof kPending | typeof kFulfilled | typeof kRejected;
  result: unknown;
}
export function getPromiseDetails(value: unknown): PromiseDetails | undefined;

export interface ProxyDetails {
  target: unknown;
  handler: unknown;
}
export function getProxyDetails(value: unknown): ProxyDetails | undefined;

export interface PreviewedEntries {
  entries: unknown[];
  isKeyValue: boolean;
}
export function previewEntries(value: unknown): PreviewedEntries | undefined;

export function getConstructorName(value: unknown): string;

export type TypedArray =
  | Uint8Array
  | Uint8ClampedArray
  | Uint16Array
  | Uint32Array
  | Int8Array
  | Int16Array
  | Int32Array
  | BigUint64Array
  | BigInt64Array
  | Float32Array
  | Float64Array;

export function isArrayBufferView(value: unknown): value is ArrayBufferView;
export function isArgumentsObject(value: unknown): value is IArguments;
export function isArrayBuffer(value: unknown): value is ArrayBuffer;
export function isAsyncFunction(value: unknown): value is Function;
export function isBigInt64Array(value: unknown): value is BigInt64Array;
export function isBigIntObject(value: unknown): value is BigInt;
export function isBigUint64Array(value: unknown): value is BigUint64Array;
export function isBooleanObject(value: unknown): value is Boolean;
export function isDataView(value: unknown): value is DataView;
export function isDate(value: unknown): value is Date;
export function isExternal(value: unknown): boolean;
export function isFloat32Array(value: unknown): value is Float32Array;
export function isFloat64Array(value: unknown): value is Float64Array;
export function isGeneratorFunction(value: unknown): value is GeneratorFunction;
export function isGeneratorObject(value: unknown): value is Generator;
export function isInt8Array(value: unknown): value is Int8Array;
export function isInt16Array(value: unknown): value is Int16Array;
export function isInt32Array(value: unknown): value is Int32Array;
export function isMap(value: unknown): value is Map<unknown, unknown>;
export function isMapIterator(value: unknown): value is IterableIterator<unknown>;
export function isModuleNamespaceObject(value: unknown): boolean;
export function isNativeError(value: unknown): value is Error;
export function isNumberObject(value: unknown): value is Number;
export function isPromise(value: unknown): value is Promise<unknown>;
export function isProxy(value: unknown): boolean;
export function isRegExp(value: unknown): value is RegExp;
export function isSet(value: unknown): value is Set<unknown>;
export function isSetIterator(value: unknown): value is IterableIterator<unknown>;
export function isSharedArrayBuffer(value: unknown): value is SharedArrayBuffer;
export function isStringObject(value: unknown): value is String;
export function isSymbolObject(value: unknown): value is Symbol;
export function isTypedArray(value: unknown): value is TypedArray;
export function isUint8Array(value: unknown): value is Uint8Array;
export function isUint8ClampedArray(value: unknown): value is Uint8ClampedArray;
export function isUint16Array(value: unknown): value is Uint16Array;
export function isUint32Array(value: unknown): value is Uint32Array;
export function isWeakMap(value: unknown): value is WeakMap<any, unknown>;
export function isWeakSet(value: unknown): value is WeakSet<any>;
export function isAnyArrayBuffer(value: unknown): value is ArrayBuffer | SharedArrayBuffer;
export function isBoxedPrimitive(value: unknown): value is Number | String | Boolean | BigInt | Symbol;

export function getBuiltinModule(id: string): any;
