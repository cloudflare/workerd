// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { Readable as _Readable, Duplex } from 'node:stream';
import type { BufferList } from 'node-internal:streams_util';

export declare class Readable extends _Readable {
  _readableState: {
    readingMore: boolean;
    autoDestroy: boolean;
    buffer: BufferList;
    dataEmitted: boolean;
    ended: boolean;
    endEmitted: boolean;
  };
}

export { ReadableState } from 'node:stream';
export const from: typeof Duplex.from;
export const fromWeb: typeof Duplex.fromWeb;
export const toWeb: typeof Duplex.toWeb;
export const wrap: typeof Duplex.wrap;
