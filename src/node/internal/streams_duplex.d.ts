import type { Duplex } from 'node:stream';
export {
  Duplex,
  Writable,
  WritableOptions,
  Readable,
  ReadableOptions,
  duplexPair,
} from 'node:stream';

export function toBYOBWeb(duplex: Duplex): {
  readable: ReadableStream;
  writable: WritableStream;
};
