import { Readable as _Readable } from 'node:stream';

export declare class Readable extends _Readable {
  destroyed: boolean;
  _readableState?: {
    readingMore: boolean;
  };
}
