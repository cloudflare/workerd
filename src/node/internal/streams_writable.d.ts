// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { Writable as _Writable } from 'node:stream';

export declare class Writable extends _Writable {
  _writableState?: {
    autoDestroy: boolean;
  };
  closed: boolean;
  destroy(err?: unknown, cb?: (err?: unknown) => void): this;
}
