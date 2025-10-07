import { Duplex } from 'node:stream';
export {
  Duplex,
  Writable,
  WritableOptions,
  Readable,
  ReadableOptions,
  duplexPair,
} from 'node:stream';
export const from: typeof Duplex.from;
export const fromWeb: typeof Duplex.fromWeb;
export const toWeb: typeof Duplex.toWeb;

export function toBYOBWeb(duplex: Duplex): {
  readable: ReadableStream;
  writable: WritableStream;
};
