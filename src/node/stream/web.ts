// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
/* eslint-disable */

const _ReadableStream = ReadableStream;
const _ReadableStreamDefaultReader = ReadableStreamDefaultReader;
const _ReadableStreamBYOBReader = ReadableStreamBYOBReader;
const _ReadableStreamBYOBRequest = ReadableStreamBYOBRequest;
const _ReadableByteStreamController = ReadableByteStreamController;
const _ReadableStreamDefaultController = ReadableStreamDefaultController;
const _TransformStream = TransformStream;
const _WritableStream = WritableStream;
const _WritableStreamDefaultWriter = WritableStreamDefaultWriter;
const _WritableStreamDefaultController = WritableStreamDefaultController;
const _ByteLengthQueuingStrategy = ByteLengthQueuingStrategy;
const _CountQueuingStrategy = CountQueuingStrategy;
const _TextEncoderStream = TextEncoderStream;
const _TextDecoderStream = TextDecoderStream;
const _CompressionStream = CompressionStream;
const _DecompressionStream = DecompressionStream;

type _ReadableStream<R = any> = ReadableStream<R>;
type _ReadableStreamDefaultReader<R = any> = ReadableStreamDefaultReader<R>;
type _ReadableStreamBYOBReader = ReadableStreamBYOBReader;
type _ReadableStreamBYOBRequest = ReadableStreamBYOBRequest;
type _ReadableByteStreamController = ReadableByteStreamController;
type _ReadableStreamDefaultController<R = any> = ReadableStreamDefaultController<R>;
type _TransformStream<I = any, O = any> = TransformStream<I, O>;
type _TransformStreamDefaultController<O = any> = TransformStreamDefaultController<O>;
type _WritableStream<W> = WritableStream<W>;
type _WritableStreamDefaultWriter<W> = WritableStreamDefaultWriter<W>;
type _WritableStreamDefaultController = WritableStreamDefaultController;
type _ByteLengthQueuingStrategy = ByteLengthQueuingStrategy;
type _CountQueuingStrategy = CountQueuingStrategy;
type _TextEncoderStream = TextEncoderStream;
type _TextDecoderStream = TextDecoderStream;
type _CompressionStream = CompressionStream;
type _DecompressionStream = DecompressionStream;

export {
  _ReadableStream as ReadableStream,
  _ReadableStreamDefaultReader as ReadableStreamDefaultReader,
  _ReadableStreamBYOBReader as ReadableStreamBYOBReader,
  _ReadableStreamBYOBRequest as ReadableStreamBYOBRequest,
  _ReadableByteStreamController as ReadableByteStreamController,
  _ReadableStreamDefaultController as ReadableStreamDefaultController,
  _TransformStream as TransformStream,
  _TransformStreamDefaultController as TransformStreamDefaultController,
  _WritableStream as WritableStream,
  _WritableStreamDefaultWriter as WritableStreamDefaultWriter,
  _WritableStreamDefaultController as WritableStreamDefaultController,
  _ByteLengthQueuingStrategy as ByteLengthQueuingStrategy,
  _CountQueuingStrategy as CountQueuingStrategy,
  _TextEncoderStream as TextEncoderStream,
  _TextDecoderStream as TextDecoderStream,
  _CompressionStream as CompressionStream,
  _DecompressionStream as DecompressionStream,
}

export default {
  ReadableStream,
  ReadableStreamDefaultReader,
  ReadableStreamBYOBReader,
  ReadableStreamBYOBRequest,
  ReadableByteStreamController,
  ReadableStreamDefaultController,
  TransformStream,
  WritableStream,
  WritableStreamDefaultWriter,
  WritableStreamDefaultController,
  ByteLengthQueuingStrategy,
  CountQueuingStrategy,
  TextEncoderStream,
  TextDecoderStream,
  CompressionStream,
  DecompressionStream,
}
