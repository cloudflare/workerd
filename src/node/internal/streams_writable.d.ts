// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { Writable as _Writable, Duplex } from 'node:stream';

export declare class Writable extends _Writable {
  _writableState: {
    autoDestroy: boolean;
    finished: boolean;
    corked: number;
  };
  closed: boolean;
  destroy(err?: unknown, cb?: (err?: unknown) => void): this;
}

export { WritableState } from 'node:stream';
export const fromWeb: typeof Duplex.fromWeb;
export const toWeb: typeof Duplex.toWeb;
