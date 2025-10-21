// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import type { OutgoingMessage } from 'node:http';
import { Writable as _Writable, Duplex } from 'node:stream';
import {
  kDestroyed,
  kIsReadable,
  kIsWritable,
} from 'node-internal:streams_util';

export declare class WritableState {
  writable?: boolean;
  ended?: boolean;
  ending?: boolean;
  finished?: boolean;
  constructed?: boolean;
  closed?: boolean;
  closeEmitted?: boolean;
  destroyed?: boolean;
  errored?: Error | null;
  errorEmitted?: boolean;
  length?: number;
  highWaterMark?: number;
  autoDestroy?: boolean;
  emitClose?: boolean;
  corked?: number;
  pendingcb?: number;
  writing?: boolean;
  needDrain?: boolean;
  prefinished?: boolean;
  finalCalled?: boolean;
  writelen?: number;
}

export declare class Writable extends _Writable {
  _writableState: WritableState;
  _readableState: undefined;
  _closed: boolean;

  destroy(err?: unknown, cb?: (err?: unknown) => void): this;
  req?: OutgoingMessage;
  errored?: Error | null;
  writableErrored: boolean;
  readableErrored: boolean;
  writableFinished?: boolean;
  writableEnded?: boolean;
  writable: boolean;
  readable: boolean;
  readableEnded: undefined;
  aborted?: boolean;

  [kDestroyed]: boolean;
  [kIsReadable]: boolean;
  [kIsWritable]: boolean;
}

export const fromWeb: typeof Duplex.fromWeb;
export const toWeb: typeof Duplex.toWeb;
