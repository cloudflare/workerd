// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Implementations of ReadableStreamSource / WritableStreamSink which wrap system streams (sockets),
// handle encoding/decoding, and optimize pumping between them when possible.

#include "http.h"

#include <workerd/api/streams/common.h>  // for StreamEncoding, ...
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/io-context.h>

namespace workerd::api {

// A ReadableStreamSource which automatically decodes its underlying stream. It does so lazily -- if
// one of the `tryRead()` overloads is never called, then a `pumpTo()` to a WritableStreamSink
// returned by `newSystemStream()` of the same encoding will not cause any decoding/encoding steps.
//
// NOTE: `inner` must be wholly-owned. In particular, it cannot contain references to JavaScript
//   heap objects, as the stream is allowed to outlive the isolate, especially in the case of
//   deferred proxying. If the inner stream for some reason contains JS references, you'll need
//   to provide your own implementation of ReadableStreamSource.
kj::Own<ReadableStreamSource> newSystemStream(kj::Own<kj::AsyncInputStream> inner,
    StreamEncoding encoding,
    IoContext& context = IoContext::current());
// Chain-taking overload: `encodings` is the stream's content coding chain in the order the
// codings were applied, and an empty array means identity. StreamEncoding::IDENTITY must never
// appear in the array -- it is not a transformation, so it has no place in a chain, and a
// violation trips a KJ_FAIL_ASSERT on the read path. getContentEncoding() maintains this
// invariant for chains parsed from headers.
kj::Own<ReadableStreamSource> newSystemStream(kj::Own<kj::AsyncInputStream> inner,
    kj::Array<StreamEncoding> encodings,
    IoContext& context = IoContext::current());

// A WritableStreamSink which automatically encodes its underlying stream.
//
// NOTE: As with the other overload of newSystemStream(), `inner` must be wholly owned.
kj::Own<WritableStreamSink> newSystemStream(kj::Own<kj::AsyncOutputStream> inner,
    StreamEncoding encoding,
    IoContext& context = IoContext::current());
// Chain-taking overload: `encodings` is the coding chain to apply to written bytes, in applied
// order, and an empty array means identity. StreamEncoding::IDENTITY must never appear in the
// array -- it is not a transformation, so it has no place in a chain, and a violation trips a
// KJ_FAIL_ASSERT on the write path. getContentEncoding() maintains this invariant for chains
// parsed from headers.
kj::Own<WritableStreamSink> newSystemStream(kj::Own<kj::AsyncOutputStream> inner,
    kj::Array<StreamEncoding> encodings,
    IoContext& context = IoContext::current());

struct SystemMultiStream {
  kj::Own<ReadableStreamSource> readable;
  kj::Own<WritableStreamSink> writable;
};

// A combo ReadableStreamSource and WritableStreamSink.
SystemMultiStream newSystemMultiStream(
    kj::Rc<kj::AsyncIoStream> stream, IoContext& context = IoContext::current());

struct ContentEncodingOptions {
  bool brotliEnabled = false;
  ContentEncodingOptions() = default;
  ContentEncodingOptions(CompatibilityFlags::Reader flags);
};

// Get the Content-Encoding header from an HttpHeaders object as a chain of StreamEncoding values,
// listed in the order they were applied. An empty array means identity. The chain currently never
// holds more than one element: the whole header value is matched against the supported codings,
// and any other value (including a comma-separated list) yields the empty chain, so the body is
// passed through unchanged, exactly as before.
kj::Array<StreamEncoding> getContentEncoding(IoContext& context,
    const kj::HttpHeaders& headers,
    Response::BodyEncoding bodyEncoding = Response::BodyEncoding::AUTO,
    ContentEncodingOptions options = {});

}  // namespace workerd::api
