export declare class ReadableByteStreamController {
  readonly byobRequest: ReadableStreamBYOBRequest | null;
  readonly desiredSize: number | null;
  enqueue(chunk: ArrayBufferView): void;
  close(): void;
  error(reason?: unknown): void;
}

export declare class ReadableStreamBYOBRequest {
  readonly view: Uint8Array | null;
  // Non-standard workerd extension: the minimum number of BYTES the source
  // must still deliver to satisfy the outstanding read (the read's `min`,
  // in bytes, less what has already been filled; the view's element size
  // for min-less reads). null once the request is invalidated.
  readonly atLeast: number | null;
  respond(bytesWritten: number): void;
  respondWithNewView(view: ArrayBufferView): void;
}

export declare class ReadableStreamDefaultController<R = unknown> {
  readonly desiredSize: number | null;
  enqueue(chunk?: R): void;
  close(): void;
  error(reason?: unknown): void;
}

export type UnderlyingSource<R = unknown> =
  | UnderlyingDefaultSource<R>
  | UnderlyingByteSource;

export interface UnderlyingDefaultSource<R = unknown> {
  start?: (
    controller: ReadableStreamDefaultController<R>
  ) => void | Promise<void>;
  pull?: (
    controller: ReadableStreamDefaultController<R>
  ) => void | Promise<void>;
  cancel?: (reason?: unknown) => void | Promise<void>;
  type?: undefined;
}

export interface UnderlyingByteSource {
  start?: (controller: ReadableByteStreamController) => void | Promise<void>;
  pull?: (controller: ReadableByteStreamController) => void | Promise<void>;
  cancel?: (reason?: unknown) => void | Promise<void>;
  type: 'bytes';
  autoAllocateChunkSize?: number;
  // Non-standard workerd extension: the TOTAL bytes this source promises
  // to produce over its lifetime (undefined = unknown). Accepts a
  // non-negative bigint or integer number (normalized to bigint). The
  // stream errors on overflow (delivering past it) or underflow (closing
  // short of it); consumer-initiated cancel is exempt. Used by the C++
  // bridge to set Content-Length instead of chunked encoding.
  expectedLength?: bigint | number;
}

export interface QueuingStrategy<T = unknown> {
  highWaterMark?: number;
  size?: (chunk: T) => number;
}

export interface ReadableStreamDefaultReader<R = unknown> {
  readonly closed: Promise<void>;
  cancel(reason?: unknown): Promise<void>;
  read(): Promise<ReadableStreamReadResult<R>>;
  releaseLock(): void;
}

export interface ReadableStreamBYOBReaderReadOptions {
  min?: number;
}

export interface ReadableStreamBYOBReader {
  readonly closed: Promise<void>;
  cancel(reason?: unknown): Promise<void>;
  read<T extends ArrayBufferView>(
    view: T,
    options?: ReadableStreamBYOBReaderReadOptions
  ): Promise<ReadableStreamReadResult<T>>;
  readAtLeast<T extends ArrayBufferView>(
    minElements: number,
    view: T
  ): Promise<ReadableStreamReadResult<T>>;
  releaseLock(): void;
}

export type ReadableStreamReader<R> =
  | ReadableStreamDefaultReader<R>
  | ReadableStreamBYOBReader;

// NOTE: done: true usually carries no value, but it is NOT always
// valueless: a BYOB read committed when the stream closes resolves with the
// partially-filled view AND done: true in a single result (spec
// CommitPullIntoDescriptor in the closed state; WPT
// readable-byte-streams/read-min.any.js "read({ min }) when closed before
// view is filled"). lib.dom models this the same way
// (ReadableStreamReadDoneResult<T> = { done: true; value?: T }). The
// explicit `| undefined` keeps `{ done: true, value: undefined }`
// assignable under exactOptionalPropertyTypes.
export type ReadableStreamReadResult<T> =
  | { done: false; value: T }
  | { done: true; value?: T | undefined };

export interface StreamPipeOptions {
  preventClose?: boolean;
  preventAbort?: boolean;
  preventCancel?: boolean;
  signal?: AbortSignal;
}

export declare class ReadableStream<R = unknown> {
  constructor(
    underlyingSource?: UnderlyingSource<R>,
    strategy?: QueuingStrategy<R>
  );
  static from<R>(
    asyncIterable: Iterable<R> | AsyncIterable<R> | R
  ): ReadableStream<R>;
  readonly locked: boolean;
  cancel(reason?: unknown): Promise<void>;
  getReader(): ReadableStreamDefaultReader<R>;
  getReader(options: { mode: 'byob' }): ReadableStreamBYOBReader;
  pipeThrough<T>(
    transform: TransformStream<R, T>,
    options?: StreamPipeOptions
  ): ReadableStream<T>;
  pipeTo(
    destination: WritableStream<R>,
    options?: StreamPipeOptions
  ): Promise<void>;
  tee(): [ReadableStream<R>, ReadableStream<R>];
  values(options?: { preventCancel?: boolean }): AsyncIterableIterator<R>;
  [Symbol.asyncIterator](options?: {
    preventCancel?: boolean;
  }): AsyncIterableIterator<R>;
}

export declare class WritableStream<W = unknown> {
  constructor(
    underlyingSink?: UnderlyingSink<W>,
    strategy?: QueuingStrategy<W>
  );
  readonly locked: boolean;
  abort(reason?: unknown): Promise<void>;
  close(): Promise<void>;
  getWriter(): WritableStreamDefaultWriter<W>;
}

export declare class WritableStreamDefaultWriter<W = unknown> {
  constructor(stream: WritableStream<W>);
  readonly closed: Promise<void>;
  readonly desiredSize: number | null;
  readonly ready: Promise<void>;
  abort(reason?: unknown): Promise<void>;
  close(): Promise<void>;
  releaseLock(): void;
  write(chunk?: W): Promise<void>;
}

export declare class WritableStreamDefaultController {
  readonly signal: AbortSignal;
  error(reason?: unknown): void;
}

export interface UnderlyingSink<W = unknown> {
  start?: (controller: WritableStreamDefaultController) => void | Promise<void>;
  write?: (
    chunk: W,
    controller: WritableStreamDefaultController
  ) => void | Promise<void>;
  close?: () => void | Promise<void>;
  abort?: (reason?: unknown) => void | Promise<void>;
  type?: undefined;
}

export declare class TransformStream<I = unknown, O = unknown> {
  constructor(
    transformer?: Transformer<I, O>,
    writableStrategy?: QueuingStrategy<I>,
    readableStrategy?: QueuingStrategy<O>
  );
  readonly readable: ReadableStream<O>;
  readonly writable: WritableStream<I>;
}

export interface Transformer<I = unknown, O = unknown> {
  start?: (
    controller: TransformStreamDefaultController<O>
  ) => void | Promise<void>;
  transform?: (
    chunk: I,
    controller: TransformStreamDefaultController<O>
  ) => void | Promise<void>;
  flush?: (
    controller: TransformStreamDefaultController<O>
  ) => void | Promise<void>;
  cancel?: (reason?: unknown) => void | Promise<void>;
  readableType?: undefined;
  writableType?: undefined;
}

export interface TransformStreamDefaultController<O = unknown> {
  readonly desiredSize: number | null;
  enqueue(chunk: O): void;
  error(reason?: unknown): void;
  terminate(): void;
}

interface PromiseWithResolvers<T> {
  promise: Promise<T>;
  resolve: (value: T | PromiseLike<T>) => void;
  reject: (reason?: unknown) => void;
}
