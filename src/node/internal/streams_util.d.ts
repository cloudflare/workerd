export { addAbortSignal } from 'node:stream';
import type { FinishedOptions } from 'node:stream';

// Exported symbols
export const kDestroyed: unique symbol;
export const kIsErrored: unique symbol;
export const kIsReadable: unique symbol;
export const kIsDisturbed: unique symbol;
export const kPaused: unique symbol;
export const kOnFinished: unique symbol;
export const kDestroy: unique symbol;
export const kConstruct: unique symbol;
export const kIsDestroyed: unique symbol;
export const kIsWritable: unique symbol;
export const kOnConstructed: unique symbol;

// Stream type definitions
interface ReadableState {
  readable?: boolean;
  ended?: boolean;
  endEmitted?: boolean;
  destroyed?: boolean;
  errored?: Error | null;
  errorEmitted?: boolean;
  closed?: boolean;
  closeEmitted?: boolean;
  constructed?: boolean;
  reading?: boolean;
  autoDestroy?: boolean;
  emitClose?: boolean;
  objectMode?: boolean;
  length?: number;
}

interface WritableState {
  writable?: boolean;
  ended?: boolean;
  finished?: boolean;
  destroyed?: boolean;
  errored?: Error | null;
  errorEmitted?: boolean;
  closed?: boolean;
  closeEmitted?: boolean;
  constructed?: boolean;
  finalCalled?: boolean;
  prefinished?: boolean;
  ending?: boolean;
  autoDestroy?: boolean;
  emitClose?: boolean;
  objectMode?: boolean;
  length?: number;
}

interface NodeStreamLike {
  _readableState?: ReadableState;
  _writableState?: WritableState;
  readable?: boolean;
  writable?: boolean;
  readableEnded?: boolean;
  writableEnded?: boolean;
  readableFinished?: boolean;
  writableFinished?: boolean;
  readableErrored?: Error | null;
  writableErrored?: Error | null;
  destroyed?: boolean;
  closed?: boolean;
  readableDidRead?: boolean;
  readableAborted?: boolean;
  _closed?: boolean;
  _defaultKeepAlive?: boolean;
  _removedConnection?: boolean;
  _removedContLen?: boolean;
  _sent100?: boolean;
  _consuming?: boolean;
  _dumped?: boolean;
  req?: object;
  aborted?: boolean;

  // Stream methods
  pipe?<T extends NodeJS.WritableStream>(
    destination: T,
    options?: { end?: boolean }
  ): T;
  on?(event: string | symbol, listener: (...args: unknown[]) => void): this;
  pause?(): this;
  resume?(): this;
  write?(
    chunk: unknown,
    encoding?: BufferEncoding,
    callback?: (error?: Error | null) => void
  ): boolean;
  emit?(event: string | symbol, ...args: unknown[]): boolean;
  once?(event: string | symbol, listener: (...args: unknown[]) => void): this;
  removeListener?(
    event: string | symbol,
    listener: (...args: unknown[]) => void
  ): this;
  destroy?(error?: Error): this;
  close?(callback?: () => void): void;
  abort?(): void;
  listenerCount?(event: string | symbol): number;

  // Internal Node.js methods
  _construct?(callback: (error?: Error | null) => void): void;
  _destroy?(
    error: Error | null,
    callback: (error?: Error | null) => void
  ): void;

  // HTTP-specific method
  setHeader?(name: string, value: string | number | readonly string[]): this;

  socket?: unknown;
  [kDestroyed]?: boolean;
  [kIsErrored]?: boolean;
  [kIsReadable]?: boolean;
  [kIsDisturbed]?: boolean;
}

// Stream detection functions
export function isReadableNodeStream(
  obj: unknown,
  strict?: boolean
): obj is NodeStreamLike;
export function isWritableNodeStream(obj: unknown): obj is NodeStreamLike;
export function isDuplexNodeStream(obj: unknown): obj is NodeStreamLike;
export function isNodeStream(obj: unknown): obj is NodeStreamLike;
export function isReadableStream(obj: unknown): obj is ReadableStream;
export function isWritableStream(obj: unknown): obj is WritableStream;
export function isIterable(obj: unknown, isAsync?: boolean): boolean;

// Stream state functions
export function isDestroyed(stream: NodeStreamLike): boolean | null;
export function isWritableEnded(stream: NodeStreamLike): boolean | null;
export function isWritableFinished(
  stream: NodeStreamLike,
  strict?: boolean
): boolean | null;
export function isReadableEnded(stream: NodeStreamLike): boolean | null;
export function isReadableFinished(
  stream: NodeStreamLike,
  strict?: boolean
): boolean | null;
export function isReadable(stream: NodeStreamLike): boolean | null;
export function isWritable(stream: NodeStreamLike): boolean | null;

interface IsFinishedOptions {
  readable?: boolean;
  writable?: boolean;
}

export function isFinished(
  stream: NodeStreamLike,
  opts?: IsFinishedOptions
): boolean | null;
export function isWritableErrored(stream: NodeStreamLike): Error | null;
export function isReadableErrored(stream: NodeStreamLike): Error | null;
export function isClosed(stream: NodeStreamLike): boolean | null;
export function isOutgoingMessage(stream: NodeStreamLike): boolean;
export function isServerResponse(stream: NodeStreamLike): boolean;
export function isServerRequest(stream: NodeStreamLike): boolean;
export function willEmitClose(stream: NodeStreamLike): boolean | null;
export function isDisturbed(stream: NodeStreamLike): boolean;
export function isErrored(stream: NodeStreamLike): boolean;

// Utility functions
export const nop: () => void;
// eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
export function once<T extends Function>(callback: T): T;

// High water mark functions
export function highWaterMarkFrom(
  options: object,
  isDuplex: boolean,
  duplexKey: string
): number | null;
export function getDefaultHighWaterMark(objectMode?: boolean): number;
export function setDefaultHighWaterMark(): never;
export function getHighWaterMark(
  state: { objectMode?: boolean },
  options: object,
  duplexKey: string,
  isDuplex: boolean
): number;

// BufferList class
export class BufferList {
  head: BufferListNode | null;
  tail: BufferListNode | null;
  length: number;

  push(v: Buffer | string): void;
  unshift(v: Buffer | string): void;
  shift(): Buffer | string | undefined;
  clear(): void;
  join(s: string): string;
  concat(n: number): Buffer;
  consume(n: number, hasStrings?: boolean): Buffer | string;
  first(): Buffer | string;
  [Symbol.iterator](): IterableIterator<Buffer | string>;
}

export interface BufferListNode {
  data: Buffer | string;
  next: BufferListNode | null;
}

type FinishedStream =
  | NodeJS.ReadableStream
  | NodeJS.WritableStream
  | NodeJS.ReadWriteStream;
type FinishedCallback = (err?: NodeJS.ErrnoException | null) => void;

export function eos(stream: FinishedStream, options: FinishedOptions): void;
export function eos(
  stream: FinishedStream,
  options: FinishedOptions,
  callback?: FinishedCallback
): void;
export function eos(stream: FinishedStream, callback?: FinishedCallback): void;

// Destroy functions
export function destroy(
  this: NodeStreamLike,
  err?: Error,
  cb?: (err?: Error) => void
): NodeStreamLike;
export function undestroy(this: NodeStreamLike): void;
export function errorOrDestroy(
  stream: NodeStreamLike,
  err: unknown,
  sync?: boolean
): void;
export function construct(
  stream: NodeStreamLike,
  cb: (err?: Error) => void
): void;
export function destroyer(stream: NodeStreamLike, err?: Error): void;
