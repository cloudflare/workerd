import type { FinishedOptions } from 'node:stream';

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
