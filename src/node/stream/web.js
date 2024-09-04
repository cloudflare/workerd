// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

export const ReadableStream = globalThis.ReadableStream;
export const ReadableStreamDefaultReader =
  globalThis.ReadableStreamDefaultReader;
export const ReadableStreamBYOBReader = globalThis.ReadableStreamBYOBReader;
export const ReadableStreamBYOBRequest = globalThis.ReadableStreamBYOBRequest;
export const ReadableByteStreamController =
  globalThis.ReadableByteStreamController;
export const ReadableStreamDefaultController =
  globalThis.ReadableStreamDefaultController;
export const TransformStream = globalThis.TransformStream;
export const TransformStreamDefaultController =
  globalThis.TransformStreamDefaultController;
export const WritableStream = globalThis.WritableStream;
export const WritableStreamDefaultWriter =
  globalThis.WritableStreamDefaultWriter;
export const WritableStreamDefaultController =
  globalThis.WritableStreamDefaultController;
export const ByteLengthQueuingStrategy = globalThis.ByteLengthQueuingStrategy;
export const CountQueuingStrategy = globalThis.CountQueuingStrategy;
export const TextEncoderStream = globalThis.TextEncoderStream;
export const TextDecoderStream = globalThis.TextDecoderStream;
export const CompressionStream = globalThis.CompressionStream;
export const DecompressionStream = globalThis.DecompressionStream;

export default {
  ReadableStream,
  ReadableStreamDefaultReader,
  ReadableStreamBYOBReader,
  ReadableStreamBYOBRequest,
  ReadableByteStreamController,
  ReadableStreamDefaultController,
  TransformStream,
  TransformStreamDefaultController,
  WritableStream,
  WritableStreamDefaultWriter,
  WritableStreamDefaultController,
  ByteLengthQueuingStrategy,
  CountQueuingStrategy,
  TextEncoderStream,
  TextDecoderStream,
  CompressionStream,
  DecompressionStream,
};
