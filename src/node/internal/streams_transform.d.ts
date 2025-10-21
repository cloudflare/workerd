// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import {
  kIsWritable,
  kDestroyed,
  kIsReadable,
} from 'node-internal:streams_util';

import { Transform as _Transform } from 'node:stream';
import type EventEmitter from 'node:events';
import type { ReadableState } from 'node-internal:streams_readable';
import type { WritableState } from 'node-internal:streams_writable';

export {
  Readable,
  Writable,
  Duplex,
  Stream,
  TransformOptions,
  TransformCallback,
  DuplexOptions,
  PassThrough,
} from 'node:stream';

export declare class Transform extends _Transform {
  _writableState: WritableState;
  _readableState: ReadableState;
  _closed: boolean;

  writableErrored: boolean;
  readableErrored: boolean;
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
  [kDestroyed]: boolean;
}
