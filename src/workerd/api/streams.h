// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Streams standard: https://streams.spec.whatwg.org/
//
// This is the most over-engineered spec...

#include "streams/readable.h"
#include "streams/writable.h"
#include "streams/transform.h"
#include "streams/compression.h"
#include "streams/encoding.h"

namespace workerd::api {

#define EW_STREAMS_ISOLATE_TYPES                                      \
  api::StreamQueuingStrategy,                                         \
  api::UnderlyingSink,                                                \
  api::UnderlyingSource,                                              \
  api::Transformer,                                                   \
  api::PipeToOptions,                                                 \
  api::ReadResult,                                                    \
  api::ReadableStream,                                                \
  api::ReadableStreamDefaultReader,                                   \
  api::ReadableStreamBYOBReader,                                      \
  api::ReadableStreamBYOBReader::ReadableStreamBYOBReaderReadOptions, \
  api::ReadableStream::GetReaderOptions,                              \
  api::ReadableStreamBYOBRequest,                                     \
  api::ReadableStreamDefaultController,                               \
  api::ReadableByteStreamController,                                  \
  api::WritableStreamDefaultController,                               \
  api::TransformStreamDefaultController,                              \
  api::ReadableStream::Transform,                                     \
  api::WritableStream,                                                \
  api::WritableStreamDefaultWriter,                                   \
  api::TransformStream,                                               \
  api::FixedLengthStream,                                             \
  api::IdentityTransformStream,                                       \
  api::IdentityTransformStream::QueuingStrategy,                      \
  api::ReadableStream::ValuesOptions,                                 \
  api::ReadableStream::ReadableStreamAsyncIterator,                   \
  api::ReadableStream::ReadableStreamAsyncIterator::Next,             \
  api::CompressionStream,                                             \
  api::DecompressionStream,                                           \
  api::TextEncoderStream,                                             \
  api::TextDecoderStream,                                             \
  api::TextDecoderStream::TextDecoderStreamInit,                      \
  api::ByteLengthQueuingStrategy,                                     \
  api::CountQueuingStrategy,                                          \
  api::QueuingStrategyInit
// The list of streams.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api
