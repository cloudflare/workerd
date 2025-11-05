import { Duplex as _Duplex } from 'node:stream';
import type {
  kIsReadable,
  kIsWritable,
  kDestroyed,
} from 'node-internal:streams_util';

export {
  Writable,
  WritableOptions,
  Readable,
  ReadableOptions,
  duplexPair,
} from 'node:stream';

export const from: typeof _Duplex.from;
export const fromWeb: typeof _Duplex.fromWeb;
export const toWeb: typeof _Duplex.toWeb;

export declare class Duplex extends _Duplex {
  _readableState: undefined;
  _writableState: undefined;
  _closed: boolean;

  [kIsReadable]: boolean;
  [kIsWritable]: boolean;
  [kDestroyed]: boolean;

  writableErrored: boolean;
  readableErrored: boolean;
}

export function toBYOBWeb(duplex: Duplex): {
  readable: ReadableStream;
  writable: WritableStream;
};
