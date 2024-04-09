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

// Derived from DefinitelyTyped https://github.com/DefinitelyTyped/DefinitelyTyped/blob/master/types/node/stream.d.ts

/* eslint-disable @typescript-eslint/no-explicit-any */

import { Stream, NodeJS, Readable, Writable } from "node-internal:streams_transform";

export {
  Stream,
  Readable,
  ReadableOptions,
  Writable,
  WritableOptions,
  Duplex,
  DuplexOptions,
  Transform,
  TransformOptions,
  PassThrough
} from "node-internal:streams_transform";

export function addAbortSignal<T extends Stream>(signal: AbortSignal, stream: T): T;

export interface FinishedOptions {
  error?: boolean | undefined;
  readable?: boolean | undefined;
  writable?: boolean | undefined;
  signal?: AbortSignal | undefined;
}

export function finished(
  stream: NodeJS.ReadableStream | NodeJS.WritableStream | NodeJS.ReadWriteStream,
  options: FinishedOptions,
  callback: (err?: NodeJS.ErrnoException | null) => void,
): () => void;
export function finished(
  stream: NodeJS.ReadableStream | NodeJS.WritableStream | NodeJS.ReadWriteStream,
  callback: (err?: NodeJS.ErrnoException | null) => void,
): () => void;

export type PipelineSourceFunction<T> = () => Iterable<T> | AsyncIterable<T>;
export type PipelineSource<T> = Iterable<T> | AsyncIterable<T> | NodeJS.ReadableStream | PipelineSourceFunction<T>;
export type PipelineTransform<S extends PipelineTransformSource<any>, U> =
  | NodeJS.ReadWriteStream
  | ((
      source: S extends (...args: any[]) => Iterable<infer ST> | AsyncIterable<infer ST> ? AsyncIterable<ST>
          : S,
  ) => AsyncIterable<U>);
export type PipelineTransformSource<T> = PipelineSource<T> | PipelineTransform<any, T>;
export type PipelineDestinationIterableFunction<T> = (source: AsyncIterable<T>) => AsyncIterable<any>;
export type PipelineDestinationPromiseFunction<T, P> = (source: AsyncIterable<T>) => Promise<P>;
export type PipelineDestination<S extends PipelineTransformSource<any>, P> = S extends
  PipelineTransformSource<infer ST> ?
      | NodeJS.WritableStream
      | PipelineDestinationIterableFunction<ST>
      | PipelineDestinationPromiseFunction<ST, P>
  : never;
export type PipelineCallback<S extends PipelineDestination<any, any>> = S extends
  PipelineDestinationPromiseFunction<any, infer P> ? (err: NodeJS.ErrnoException | null, value: P) => void
  : (err: NodeJS.ErrnoException | null) => void;
export type PipelinePromise<S extends PipelineDestination<any, any>> = S extends
  PipelineDestinationPromiseFunction<any, infer P> ? Promise<P> : Promise<void>;
export interface PipelineOptions {
  signal?: AbortSignal | undefined;
  end?: boolean | undefined;
}

export function pipeline<A extends PipelineSource<any>, B extends PipelineDestination<A, any>>(
  source: A,
  destination: B,
  callback?: PipelineCallback<B>,
): B extends NodeJS.WritableStream ? B : NodeJS.WritableStream;
export function pipeline<
  A extends PipelineSource<any>,
  T1 extends PipelineTransform<A, any>,
  B extends PipelineDestination<T1, any>,
>(
  source: A,
  transform1: T1,
  destination: B,
  callback?: PipelineCallback<B>,
): B extends NodeJS.WritableStream ? B : NodeJS.WritableStream;
export function pipeline<
  A extends PipelineSource<any>,
  T1 extends PipelineTransform<A, any>,
  T2 extends PipelineTransform<T1, any>,
  B extends PipelineDestination<T2, any>,
>(
  source: A,
  transform1: T1,
  transform2: T2,
  destination: B,
  callback?: PipelineCallback<B>,
): B extends NodeJS.WritableStream ? B : NodeJS.WritableStream;
export function pipeline<
  A extends PipelineSource<any>,
  T1 extends PipelineTransform<A, any>,
  T2 extends PipelineTransform<T1, any>,
  T3 extends PipelineTransform<T2, any>,
  B extends PipelineDestination<T3, any>,
>(
  source: A,
  transform1: T1,
  transform2: T2,
  transform3: T3,
  destination: B,
  callback?: PipelineCallback<B>,
): B extends NodeJS.WritableStream ? B : NodeJS.WritableStream;
export function pipeline<
  A extends PipelineSource<any>,
  T1 extends PipelineTransform<A, any>,
  T2 extends PipelineTransform<T1, any>,
  T3 extends PipelineTransform<T2, any>,
  T4 extends PipelineTransform<T3, any>,
  B extends PipelineDestination<T4, any>,
>(
  source: A,
  transform1: T1,
  transform2: T2,
  transform3: T3,
  transform4: T4,
  destination: B,
  callback?: PipelineCallback<B>,
): B extends NodeJS.WritableStream ? B : NodeJS.WritableStream;
export function pipeline(
  streams: ReadonlyArray<NodeJS.ReadableStream | NodeJS.WritableStream | NodeJS.ReadWriteStream>,
  callback?: (err: NodeJS.ErrnoException | null) => void,
): NodeJS.WritableStream;
export function pipeline(
  stream1: NodeJS.ReadableStream,
  stream2: NodeJS.ReadWriteStream | NodeJS.WritableStream,
  ...streams: Array<
      NodeJS.ReadWriteStream | NodeJS.WritableStream | ((err: NodeJS.ErrnoException | null) => void)
  >
): NodeJS.WritableStream;

export interface Pipe {
  close(): void;
  hasRef(): boolean;
  ref(): void;
  unref(): void;
}

export function isErrored(stream: Readable | Writable | NodeJS.ReadableStream | NodeJS.WritableStream): boolean;
export function isReadable(stream: Readable | NodeJS.ReadableStream): boolean;
