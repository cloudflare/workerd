// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

import { pipeline } from 'node-internal:streams_pipeline';
import { addAbortSignal } from 'node-internal:streams_add_abort_signal';
import { eos } from 'node-internal:streams_end_of_stream';
import {
  isErrored,
  isDisturbed,
  isDestroyed,
  isReadable,
  isWritable,
} from 'node-internal:streams_util';
import { destroyer } from 'node-internal:streams_destroy';
import {
  getDefaultHighWaterMark,
  setDefaultHighWaterMark,
} from 'node-internal:streams_state';
import { compose } from 'node-internal:streams_compose';
import { Stream } from 'node-internal:streams_legacy';
import { Writable } from 'node-internal:streams_writable';
import { Readable } from 'node-internal:streams_readable';
import { Duplex, duplexPair } from 'node-internal:streams_duplex';
import { Transform, PassThrough } from 'node-internal:streams_transform';
import { promises } from 'node-internal:streams_promises';

// eslint-disable-next-line @typescript-eslint/unbound-method
export const _isArrayBufferView = Stream._isArrayBufferView;
// eslint-disable-next-line @typescript-eslint/unbound-method
export const _isUint8Array = Stream._isUint8Array;
// eslint-disable-next-line @typescript-eslint/unbound-method
export const _uint8ArrayToBuffer = Stream._uint8ArrayToBuffer;
const destroy = destroyer;
const finished = eos;

export {
  addAbortSignal,
  compose,
  destroy,
  finished,
  isErrored,
  isDisturbed,
  isReadable,
  pipeline,
  Stream,
  Writable,
  Readable,
  Duplex,
  Transform,
  PassThrough,
  promises,
  getDefaultHighWaterMark,
  setDefaultHighWaterMark,
  isDestroyed,
  isWritable,
  duplexPair,
};

// @ts-expect-error TS2322 Type incompatibility between internal and Node.js stream types
Stream.addAbortSignal = addAbortSignal;
// eslint-disable-next-line @typescript-eslint/no-unsafe-assignment
Stream.compose = compose;
Stream.destroy = destroy;
// @ts-expect-error TS2741 __promisify__ is missing.
Stream.finished = finished;
Stream.isReadable = isReadable;
Stream.isWritable = isWritable;
Stream.isErrored = isErrored;
Stream.isDisturbed = isDisturbed;
Stream.pipeline = pipeline;
Stream.Stream = Stream;
// @ts-expect-error TS2419 Type incompatibility due to exactOptionalPropertyTypes
Stream.Writable = Writable;
// @ts-expect-error TS2419 Type incompatibility due to exactOptionalPropertyTypes
Stream.Readable = Readable;
Stream.Duplex = Duplex;
// @ts-expect-error TS2419 Type incompatibility due to exactOptionalPropertyTypes
Stream.Transform = Transform;
Stream.PassThrough = PassThrough;
// @ts-expect-error TS2322 Type incompatibility due to exactOptionalPropertyTypes
Stream.promises = promises;
Stream.getDefaultHighWaterMark = getDefaultHighWaterMark;
Stream.setDefaultHighWaterMark = setDefaultHighWaterMark;
Stream.isDestroyed = isDestroyed;
Stream.duplexPair = duplexPair;

export default Stream;
