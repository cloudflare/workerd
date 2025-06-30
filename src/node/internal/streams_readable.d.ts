// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { Readable as _Readable } from 'node:stream';

export declare class Readable extends _Readable {
  _readableState?: {
    readingMore: boolean;
  };
}
