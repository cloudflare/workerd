import { Readable as _Readable } from 'node:stream';

export declare class Readable extends _Readable {
  public destroyed: boolean;
  public _readableState?: {
    readingMore: boolean;
  };
}
