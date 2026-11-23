// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/headers.h>
#include <workerd/api/js-readable-stream.h>
#include <workerd/api/js-writable-stream.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

// Decodes the aws-chunked content encoding of S3 streaming uploads (Content-Encoding:
// aws-chunked, x-amz-content-sha256: STREAMING-...). The framing is that of HTTP/1.1 chunked
// transfer coding: `hex-size[;extensions] CRLF data CRLF` per chunk, then a zero-size chunk,
// trailer fields, and a final CRLF. The trailer usually carries the payload checksum.
//
// Chunk extensions, such as chunk-signature, are checked against RFC 9112's grammar and skipped:
// chunk signatures are not verified. A trailer line may end in LF before its CRLF, as S3 allows. Both sides are native streams, so a pipe through the decoder between two other
// native streams never runs JavaScript.
//
// The decoding itself is the Rust crate in aws-chunked/; this class and its stage connect it to
// the streams.
//
// Experimental: exposed only with the "experimental" compatibility flag.
class AwsChunkedDecoderStream: public jsg::Object {
 public:
  struct Options {
    // The decoded length (x-amz-decoded-content-length). If set, any other length is an error.
    jsg::Optional<double> decodedLength;
    // The trailer fields to keep, case-insensitively. Others are dropped. Defaults to all.
    jsg::Optional<kj::Array<kj::String>> allowedTrailers;
    // The most bytes the trailer section may take. Defaults to 16 KiB.
    jsg::Optional<double> maxTrailerBytes;
    // If true (the default), invalid input is still read to its end before the stream errors,
    // so that a server can respond to the client rather than resetting the connection.
    jsg::Optional<bool> drainOnError;

    JSG_STRUCT(decodedLength, allowedTrailers, maxTrailerBytes, drainOnError);
  };

  AwsChunkedDecoderStream(JsReadableStream readable,
      JsWritableStream writable,
      jsg::Promise<jsg::Ref<Headers>> trailers)
      : readable(kj::mv(readable)),
        writable(kj::mv(writable)),
        trailers(kj::mv(trailers)) {}

  static jsg::Ref<AwsChunkedDecoderStream> constructor(
      jsg::Lock& js, jsg::Optional<Options> options);

  JsReadableStream getReadable(jsg::Lock& js) {
    return readable.addRef(js);
  }
  JsWritableStream getWritable(jsg::Lock& js) {
    return writable.addRef(js);
  }
  // Resolves with the trailer fields once the input has been decoded, and rejects if it can't
  // be.
  jsg::MemoizedIdentity<jsg::Promise<jsg::Ref<Headers>>>& getTrailers() {
    return trailers;
  }

  JSG_RESOURCE_TYPE(AwsChunkedDecoderStream) {
    JSG_READONLY_PROTOTYPE_PROPERTY(readable, getReadable);
    JSG_READONLY_PROTOTYPE_PROPERTY(writable, getWritable);
    JSG_READONLY_PROTOTYPE_PROPERTY(trailers, getTrailers);

    JSG_TS_OVERRIDE({
      constructor(options?: AwsChunkedDecoderStreamOptions);
      readonly readable: ReadableStream<Uint8Array>;
      readonly writable: WritableStream<ArrayBuffer | ArrayBufferView>;
      readonly trailers: Promise<Headers>;
    });
  }

 private:
  JsReadableStream readable;
  JsWritableStream writable;
  jsg::MemoizedIdentity<jsg::Promise<jsg::Ref<Headers>>> trailers;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(readable, writable, trailers);
  }
};

}  // namespace workerd::api
