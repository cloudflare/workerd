import { Readable } from 'node-internal:streams_readable';
import { Writable } from 'node-internal:streams_writable';

export type ReadStreamOptions = {
  autoDestroy?: boolean;
  fd?: number;
  flags?: string;
  highWaterMark?: number;
  mode?: number;
};

// @ts-expect-error TS2323 Cannot redeclare.
export declare class ReadStream extends Readable {
  fd: number | null;
  flags: string;
  path: string;
  mode: number;

  constructor(path: string, options?: Record<string, unknown>);
  close(callback: VoidFunction): void;
}

// @ts-expect-error TS2323 Cannot redeclare.
export function ReadStream(
  this: unknown,
  path: string,
  options?: ReadStreamOptions
): ReadStream {
  if (!(this instanceof ReadStream)) {
    return new ReadStream(path, options);
  }

  // TODO(soon): Implement this.

  Reflect.apply(Readable, this, [options]);
  return this;
}
Object.setPrototypeOf(ReadStream.prototype, Readable.prototype);
Object.setPrototypeOf(ReadStream, Readable);

Object.defineProperty(ReadStream.prototype, 'autoClose', {
  get(this: ReadStream): boolean {
    // TODO(soon): Implement this.
    return false;
  },
  set(this: ReadStream, _val: boolean): void {
    // TODO(soon): Implement this.
  },
});

ReadStream.prototype.close = function (_cb: VoidFunction): void {
  throw new Error('Not implemented');
};

Object.defineProperty(ReadStream.prototype, 'pending', {
  get(this: ReadStream): boolean {
    return this.fd === null;
  },
  configurable: true,
});

export type WriteStreamOptions = {
  flags?: string;
  encoding?: string;
  fd?: number;
  mode?: number;
  autoClose?: boolean;
};

// @ts-expect-error TS2323 Cannot redeclare.
export declare class WriteStream extends Writable {
  constructor(path: string, options?: WriteStreamOptions);
  close(cb: VoidFunction): void;
  destroySoon(): void;
}

// @ts-expect-error TS2323 Cannot redeclare.
export function WriteStream(
  this: unknown,
  path: string,
  options?: WriteStreamOptions
): WriteStream {
  if (!(this instanceof WriteStream)) {
    return new WriteStream(path, options);
  }

  // TODO(soon): Implement this.

  Reflect.apply(Writable, this, [options]);
  return this;
}
Object.setPrototypeOf(WriteStream.prototype, Writable.prototype);
Object.setPrototypeOf(WriteStream, Writable);

Object.defineProperty(WriteStream.prototype, 'autoClose', {
  get(this: WriteStream): boolean {
    return false;
  },
  set(this: WriteStream, _val: boolean): void {
    // TODO(soon): implement this
  },
});

WriteStream.prototype.close = function (
  this: typeof WriteStream,
  _cb: VoidFunction
): void {
  throw new Error('Not implemented');
};

// There is no shutdown() for files.
// eslint-disable-next-line @typescript-eslint/unbound-method
WriteStream.prototype.destroySoon = WriteStream.prototype.end;

Object.defineProperty(WriteStream.prototype, 'pending', {
  get(this: WriteStream): boolean {
    // TODO(soon): Implement this
    return false;
  },
  configurable: true,
});
