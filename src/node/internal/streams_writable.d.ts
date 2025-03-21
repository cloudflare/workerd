/* This project is licensed under the MIT license.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE. */

// Derived from DefinitelyTyped https://github.com/DefinitelyTyped/DefinitelyTyped/blob/master/types/node/stream.d.ts#L1060

/* TODO: the following is adopted code, enabling linting one day */
/* eslint-disable */

import { EventEmitter } from 'node:events';

interface WritableOptions {
  highWaterMark?: number | undefined;
  decodeStrings?: boolean | undefined;
  defaultEncoding?: BufferEncoding | undefined;
  objectMode?: boolean | undefined;
  emitClose?: boolean | undefined;
  write?(
    this: Writable,
    chunk: any,
    encoding: BufferEncoding,
    callback: (error?: Error | null) => void
  ): void;
  writev?(
    this: Writable,
    chunks: Array<{ chunk: any; encoding: BufferEncoding }>,
    callback: (error?: Error | null) => void
  ): void;
  destroy?(
    this: Writable,
    error: Error | null,
    callback: (error: Error | null) => void
  ): void;
  final?(this: Writable, callback: (error?: Error | null) => void): void;
  autoDestroy?: boolean | undefined;
}

export class internal extends EventEmitter {
  pipe<T extends NodeJS.WritableStream>(
    destination: T,
    options?: { end?: boolean | undefined }
  ): T;
}

export class Stream extends internal {
  constructor(opts?: any);
}

export class Writable extends Stream implements NodeJS.WritableStream {
  readonly writable: boolean;
  readonly writableEnded: boolean;
  readonly writableFinished: boolean;
  readonly writableHighWaterMark: number;
  readonly writableLength: number;
  readonly writableObjectMode: boolean;
  readonly writableCorked: number;
  destroyed: boolean;
  constructor(opts?: WritableOptions);
  _write(
    chunk: any,
    encoding: BufferEncoding,
    callback: (error?: Error | null) => void
  ): void;
  _writev?(
    chunks: Array<{ chunk: any; encoding: BufferEncoding }>,
    callback: (error?: Error | null) => void
  ): void;
  _destroy(error: Error | null, callback: (error?: Error | null) => void): void;
  _final(callback: (error?: Error | null) => void): void;
  write(chunk: any, cb?: (error: Error | null | undefined) => void): boolean;
  write(
    chunk: any,
    encoding: BufferEncoding,
    cb?: (error: Error | null | undefined) => void
  ): boolean;
  setDefaultEncoding(encoding: BufferEncoding): this;
  end(cb?: () => void): this;
  end(chunk: any, cb?: () => void): this;
  end(chunk: any, encoding: BufferEncoding, cb?: () => void): this;
  cork(): void;
  uncork(): void;
  destroy(error?: Error): this;

  addListener(event: 'close', listener: () => void): this;
  addListener(event: 'drain', listener: () => void): this;
  addListener(event: 'error', listener: (err: Error) => void): this;
  addListener(event: 'finish', listener: () => void): this;
  addListener(event: 'pipe', listener: (src: any) => void): this;
  addListener(event: 'unpipe', listener: (src: any) => void): this;
  addListener(event: string | symbol, listener: (...args: any[]) => void): this;

  emit(event: 'close'): boolean;
  emit(event: 'drain'): boolean;
  emit(event: 'error', err: Error): boolean;
  emit(event: 'finish'): boolean;
  emit(event: 'pipe', src: any): boolean;
  emit(event: 'unpipe', src: any): boolean;
  emit(event: string | symbol, ...args: any[]): boolean;

  on(event: 'close', listener: () => void): this;
  on(event: 'drain', listener: () => void): this;
  on(event: 'error', listener: (err: Error) => void): this;
  on(event: 'finish', listener: () => void): this;
  on(event: 'pipe', listener: (src: any) => void): this;
  on(event: 'unpipe', listener: (src: any) => void): this;
  on(event: string | symbol, listener: (...args: any[]) => void): this;

  once(event: 'close', listener: () => void): this;
  once(event: 'drain', listener: () => void): this;
  once(event: 'error', listener: (err: Error) => void): this;
  once(event: 'finish', listener: () => void): this;
  once(event: 'pipe', listener: (src: any) => void): this;
  once(event: 'unpipe', listener: (src: any) => void): this;
  once(event: string | symbol, listener: (...args: any[]) => void): this;

  prependListener(event: 'close', listener: () => void): this;
  prependListener(event: 'drain', listener: () => void): this;
  prependListener(event: 'error', listener: (err: Error) => void): this;
  prependListener(event: 'finish', listener: () => void): this;
  prependListener(event: 'pipe', listener: (src: any) => void): this;
  prependListener(event: 'unpipe', listener: (src: any) => void): this;
  prependListener(
    event: string | symbol,
    listener: (...args: any[]) => void
  ): this;

  prependOnceListener(event: 'close', listener: () => void): this;
  prependOnceListener(event: 'drain', listener: () => void): this;
  prependOnceListener(event: 'error', listener: (err: Error) => void): this;
  prependOnceListener(event: 'finish', listener: () => void): this;
  prependOnceListener(event: 'pipe', listener: (src: any) => void): this;
  prependOnceListener(event: 'unpipe', listener: (src: any) => void): this;
  prependOnceListener(
    event: string | symbol,
    listener: (...args: any[]) => void
  ): this;

  removeListener(event: 'close', listener: () => void): this;
  removeListener(event: 'drain', listener: () => void): this;
  removeListener(event: 'error', listener: (err: Error) => void): this;
  removeListener(event: 'finish', listener: () => void): this;
  removeListener(event: 'pipe', listener: (src: any) => void): this;
  removeListener(event: 'unpipe', listener: (src: any) => void): this;
  removeListener(
    event: string | symbol,
    listener: (...args: any[]) => void
  ): this;
}
