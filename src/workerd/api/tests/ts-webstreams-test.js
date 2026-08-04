// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0  workerd/require-copyright-header
const {
  ReadableStream,
  ReadableStreamDefaultReader,
  ReadableStreamBYOBReader,
  ReadableStreamDefaultController,
  ReadableByteStreamController,
  ReadableStreamBYOBRequest,
  ByteLengthQueuingStrategy,
  CountQueuingStrategy,
  WritableStream,
  WritableStreamDefaultWriter,
  WritableStreamDefaultController,
  TransformStream,
  TransformStreamDefaultController,
  IdentityTransformStream,
  FixedLengthStream,
  TextEncoderStream,
  TextDecoderStream,
} = globalThis;

import { doesNotMatch } from 'node:assert';

export const existenceTest = {
  test() {
    // None of the classes should report as "native code"
    doesNotMatch(ReadableStream.toString(), /\[native code\]/);
    doesNotMatch(ReadableStreamDefaultReader.toString(), /\[native code\]/);
    doesNotMatch(ReadableStreamBYOBReader.toString(), /\[native code\]/);
    doesNotMatch(ReadableStreamDefaultController.toString(), /\[native code\]/);
    doesNotMatch(ReadableByteStreamController.toString(), /\[native code\]/);
    doesNotMatch(ReadableStreamBYOBRequest.toString(), /\[native code\]/);
    doesNotMatch(ByteLengthQueuingStrategy.toString(), /\[native code\]/);
    doesNotMatch(CountQueuingStrategy.toString(), /\[native code\]/);
    doesNotMatch(WritableStream.toString(), /\[native code\]/);
    doesNotMatch(WritableStreamDefaultWriter.toString(), /\[native code\]/);
    doesNotMatch(WritableStreamDefaultController.toString(), /\[native code\]/);
    doesNotMatch(TransformStream.toString(), /\[native code\]/);
    doesNotMatch(
      TransformStreamDefaultController.toString(),
      /\[native code\]/
    );
    doesNotMatch(IdentityTransformStream.toString(), /\[native code\]/);
    doesNotMatch(FixedLengthStream.toString(), /\[native code\]/);
    doesNotMatch(TextEncoderStream.toString(), /\[native code\]/);
    doesNotMatch(TextDecoderStream.toString(), /\[native code\]/);
  },
};
