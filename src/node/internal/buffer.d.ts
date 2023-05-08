// Type definitions for c++ implementation

interface CompareOptions {
  aStart?: number;
  aEnd?: number;
  bStart?: number;
  bEnd?: number
}

type BufferSource = ArrayBufferView | ArrayBuffer;

export function byteLength(value: string): number;
export function compare(a: Uint8Array, b: Uint8Array, options?: CompareOptions): number;
export function concat(list: Uint8Array[], length: number): ArrayBuffer;
export function decodeString(value: string, encoding: string): ArrayBuffer;
export function fillImpl(buffer: Uint8Array,
                         value: string | BufferSource,
                         start: number,
                         end: number,
                         encoding?: string): void;
export function indexOf(buffer: Uint8Array,
                        value: string | Uint8Array,
                        byteOffset?: number,
                        encoding?: string,
                        findLast?: boolean): number | undefined;
export function swap(buffer: Uint8Array, size: 16|32|64): void;
export function toString(buffer: Uint8Array,
                         start: number,
                         end: number,
                         encoding: string): string;
export function write(buffer: Uint8Array,
                      value: string,
                      offset: number,
                      length: number,
                      encoding: string): void;
export function decode(buffer: Uint8Array, state: Uint8Array): string;
export function flush(state: Uint8Array): string;
