// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

/* eslint-disable */

// TODO(soon): Remove this once assert is out of experimental
import { default as CompatibilityFlags } from 'workerd:compatibility-flags';
if (!CompatibilityFlags.workerdExperimental) {
  throw new Error('node:stream is experimental.');
}

import { pipeline } from 'node-internal:streams_pipeline';
import {
  destroyer,
  eos,
  isErrored,
  isDisturbed,
  isReadable,
  addAbortSignal,
} from 'node-internal:streams_util';
import {
  compose,
} from 'node-internal:streams_compose';
import { Stream } from 'node-internal:streams_legacy';
import { Writable } from 'node-internal:streams_writable';
import { Readable } from 'node-internal:streams_readable';
import { Duplex } from 'node-internal:streams_duplex';
import { Transform, PassThrough } from 'node-internal:streams_transform';
import { promises } from 'node-internal:streams_promises';

export const _isUint8Array = Stream._isUint8Array;
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
};

Stream.addAbortSignal = addAbortSignal;
Stream.compose = compose;
Stream.destroy = destroy;
Stream.finished = finished;
Stream.isReadable = isReadable;
Stream.isErrored = isErrored;
Stream.isErrored = isDisturbed;
Stream.pipeline = pipeline;
Stream.Stream = Stream;
Stream.Writable = Writable;
Stream.Readable = Readable;
Stream.Duplex = Duplex;
Stream.Transform = Transform;
Stream.PassThrough = PassThrough;
Stream.promises = promises;

export default Stream;
