// Type definitions for c++ implementation

interface CompareOptions {
  aStart?: number;
  aEnd?: number;
  bStart?: number;
  bEnd?: number
}

type BufferSource = ArrayBufferView | ArrayBuffer;

export type Encoding = number;

export function byteLength(value: string): number;
export function compare(a: Uint8Array, b: Uint8Array, options?: CompareOptions): number;
export function concat(list: Uint8Array[], length: number): ArrayBuffer;
export function decodeString(value: string, encoding: Encoding): ArrayBuffer;
export function fillImpl(buffer: Uint8Array,
                         value: string | BufferSource,
                         start: number,
                         end: number,
                         encoding?: Encoding): void;
export function indexOf(buffer: Uint8Array,
                        value: string | Uint8Array,
                        byteOffset?: number,
                        encoding?: Encoding,
                        findLast?: boolean): number | undefined;
export function swap(buffer: Uint8Array, size: 16|32|64): void;
export function toString(buffer: Uint8Array,
                         start: number,
                         end: number,
                         encoding: Encoding): string;
export function write(buffer: Uint8Array,
                      value: string,
                      offset: number,
                      length: number,
                      encoding: Encoding): void;
export function decode(buffer: Uint8Array, state: Uint8Array): string;
export function flush(state: Uint8Array): string;
export function isAscii(value: ArrayBufferView): boolean;
export function isUtf8(value: ArrayBufferView): boolean;
export function transcode(source: ArrayBufferView, fromEncoding: Encoding, toEncoding: Encoding): ArrayBuffer;

export const ASCII: Encoding;
export const LATIN1: Encoding;
export const UTF8: Encoding;
export const UTF16LE: Encoding;
export const BASE64: Encoding;
export const BASE64URL: Encoding;
export const HEX: Encoding;
