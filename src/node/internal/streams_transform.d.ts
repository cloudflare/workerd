// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import type EventEmitter from 'node:events';
import { Transform as _Transform } from 'node:stream';
import type { ReadableState } from 'node-internal:streams_readable';
import type { WritableState } from 'node-internal:streams_writable';
import {
  kIsWritable,
  kIsReadable,
  kIsDestroyed,
} from 'node-internal:streams_util';

export {
  Readable,
  Writable,
  Duplex,
  Transform as _Transform,
  Stream,
  TransformOptions,
  TransformCallback,
  DuplexOptions,
  PassThrough,
} from 'node:stream';

export declare class Transform extends _Transform {
  _writableState: WritableState;
  _readableState: ReadableState;
  writableErrored: boolean;
  readableErrored: boolean;
  _closed: boolean;
  readable: boolean;
  writable: boolean;
  errored?: Error | null;
  readableFinished?: boolean;
  writableFinished?: boolean;
  writableEnded?: boolean;
  aborted?: boolean;
  req?: EventEmitter;

  [kIsWritable]: boolean;
  [kIsReadable]: boolean;
  [kIsDestroyed]: boolean;
}
