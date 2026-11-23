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
// transfer coding: `hex-size[;chunk-signature=sig] CRLF data CRLF` per chunk, then a zero-size
// chunk, trailer fields, and a final CRLF. The trailer usually carries the payload checksum.
//
// A signed body (STREAMING-AWS4-HMAC-SHA256-PAYLOAD[-TRAILER]) is verified: given the request's
// credentials and signature, every chunk signature, and the trailer signature if there is a
// trailer, must match, and a chunk is output only once its signature has been checked. An
// unsigned body (STREAMING-UNSIGNED-PAYLOAD-TRAILER) may carry no signatures, and no chunk
// extension of any other kind. Trailer field names are lowercased, and a trailer line may end in
// LF before its CRLF, as S3 allows. Both sides are native streams, so a pipe through the decoder
// between two other native streams never runs JavaScript.
//
// The decoding is the s3s-chunked Rust crate's, driven by the crate in aws-chunked/; this class
// and its stage connect it to the streams.
//
// Experimental: exposed only with the "experimental" compatibility flag.
class AwsChunkedDecoderStream: public jsg::Object {
 public:
  // What a signed body is verified with: the credentials and scope of the request's SigV4
  // signature, and that signature, which the chunk signatures chain from.
  struct SigningOptions {
    kj::String secretAccessKey;
    // The request's x-amz-date, such as "20130524T000000Z".
    kj::String amzDate;
    kj::String region;
    kj::String service;
    // The request's signature, as 64 hex digits.
    kj::String seedSignature;

    JSG_STRUCT(secretAccessKey, amzDate, region, service, seedSignature);
  };

  struct Options {
    // The decoded length (x-amz-decoded-content-length). Any other length is an error.
    double decodedLength;
    // The trailer fields to keep, case-insensitively. Others are dropped. Defaults to all.
    jsg::Optional<kj::Array<kj::String>> allowedTrailers;
    // The most bytes the trailer section may take. Defaults to 16 KiB.
    jsg::Optional<double> maxTrailerBytes;
    // If true (the default), invalid input is still read to its end before the stream errors,
    // so that a server can respond to the client rather than resetting the connection.
    jsg::Optional<bool> drainOnError;
    // Given for a signed body, which is then verified. Without it, a body that carries
    // signatures is an error.
    jsg::Optional<SigningOptions> signing;

    JSG_STRUCT(decodedLength, allowedTrailers, maxTrailerBytes, drainOnError, signing);
  };

  AwsChunkedDecoderStream(JsReadableStream readable,
      JsWritableStream writable,
      jsg::Promise<jsg::Ref<Headers>> trailers)
      : readable(kj::mv(readable)),
        writable(kj::mv(writable)),
        trailers(kj::mv(trailers)) {}

  static jsg::Ref<AwsChunkedDecoderStream> constructor(jsg::Lock& js, Options options);

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
      constructor(options: AwsChunkedDecoderStreamOptions);
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
