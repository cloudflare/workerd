// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "aws-chunked.h"

#include "sync-transform.h"

#include <workerd/api/headers.h>
#include <workerd/api/streams/aws-chunked/lib.rs.h>
#include <workerd/io/io-context.h>

#include <kj-rs/convert.h>

namespace workerd::api {

namespace {

using RustDecoder = ::rust::Box<aws_chunked::Decoder>;
using Trailers = ::rust::Vec<aws_chunked::TrailerField>;

// Runs the Rust decoder (`aws-chunked/lib.rs`), settling the trailers when it ends, fails, or is
// abandoned.
class AwsChunkedDecodeStage final: public SyncTransformStage {
 public:
  AwsChunkedDecodeStage(RustDecoder decoder, kj::Own<kj::PromiseFulfiller<Trailers>> fulfiller)
      : decoder(kj::mv(decoder)),
        trailersFulfiller(kj::mv(fulfiller)) {}

  ~AwsChunkedDecodeStage() noexcept(false) {
    trailersFulfiller->reject(
        JSG_KJ_EXCEPTION(DISCONNECTED, TypeError, "The aws-chunked stream did not finish."));
  }

  void push(kj::ArrayPtr<const kj::byte> input, Output& output) override {
    auto maybeException =
        kj::runCatchingExceptions([&]() { decoder->push(input.as<kj_rs::Rust>()); });
    // The data decoded before a failure is still output.
    takeOutput(output);
    KJ_IF_SOME(exception, maybeException) {
      trailersFulfiller->reject(exception.clone());
      kj::throwFatalException(kj::mv(exception));
    }
  }

  void end(Output& output) override {
    KJ_IF_SOME(exception,
        kj::runCatchingExceptions([&]() { trailersFulfiller->fulfill(decoder->end()); })) {
      trailersFulfiller->reject(exception.clone());
      kj::throwFatalException(kj::mv(exception));
    }
  }

  void cancel(const kj::Exception& reason) override {
    trailersFulfiller->reject(reason.clone());
  }

  kj::Maybe<uint64_t> getOutputLength() const override {
    return decoder->decoded_length();
  }

  size_t retainedBytes() const override {
    return decoder->retained_bytes();
  }

 private:
  RustDecoder decoder;
  kj::Own<kj::PromiseFulfiller<Trailers>> trailersFulfiller;

  // Outputs the decoded data without copying it: each piece stays in the Rust buffer it views
  // until the stream is done with it.
  void takeOutput(Output& output) {
    while (decoder->has_output()) {
      auto piece = decoder->pop_output();
      auto data = piece->data();
      // The readable side never writes to its chunks.
      auto view = kj::arrayPtr(const_cast<kj::byte*>(data.data()), data.size());
      output.add(view.attach(kj::mv(piece)));
    }
  }
};

::rust::String toRust(kj::StringPtr text) {
  return ::rust::String(text.begin(), text.size());
}

aws_chunked::DecoderOptions toRustOptions(AwsChunkedDecoderStream::Options& options) {
  aws_chunked::DecoderOptions result{
    .decoded_length = options.decodedLength,
    .has_allowed_trailers = options.allowedTrailers != kj::none,
    .allowed_trailers = {},
    .has_max_trailer_bytes = options.maxTrailerBytes != kj::none,
    .max_trailer_bytes = options.maxTrailerBytes.orDefault(0),
    .drain_on_error = options.drainOnError.orDefault(true),
    .has_signing = options.signing != kj::none,
    .signing = {},
  };
  KJ_IF_SOME(allowed, options.allowedTrailers) {
    for (auto& name: allowed) {
      result.allowed_trailers.push_back(toRust(name));
    }
  }
  KJ_IF_SOME(signing, options.signing) {
    result.signing = aws_chunked::SigningOptions{
      .secret_access_key = toRust(signing.secretAccessKey),
      .amz_date = toRust(signing.amzDate),
      .region = toRust(signing.region),
      .service = toRust(signing.service),
      .seed_signature = toRust(signing.seedSignature),
    };
  }
  return result;
}

}  // namespace

jsg::Ref<AwsChunkedDecoderStream> AwsChunkedDecoderStream::constructor(
    jsg::Lock& js, Options options) {
  auto decoder = aws_chunked::new_decoder(toRustOptions(options));

  auto& ioContext = IoContext::current();
  auto paf = kj::newPromiseAndFulfiller<Trailers>();
  auto stage = kj::heap<AwsChunkedDecodeStage>(kj::mv(decoder), kj::mv(paf.fulfiller));
  // Up to one network read of decoded data may wait to be read.
  auto streams = newSyncTransformStreams(js, ioContext, kj::mv(stage), 64 * 1024);

  // The trailers are settled by the writes, so waiting for them is not I/O of its own: if
  // nothing writes, the request should be seen to hang rather than wait on them.
  auto trailers =
      ioContext.awaitIoLegacy(js, kj::mv(paf.promise)).then(js, [](jsg::Lock& js, Trailers fields) {
    auto headers = js.alloc<Headers>();
    for (auto& field: fields) {
      headers->append(js, kj::str(field.name), kj::str(field.value));
    }
    return headers;
  });
  // A caller that only reads the body need not wait for the trailers.
  trailers.markAsHandled(js);

  return js.alloc<AwsChunkedDecoderStream>(
      kj::mv(streams.readable), kj::mv(streams.writable), kj::mv(trailers));
}

}  // namespace workerd::api
