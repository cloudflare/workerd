// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "system-streams.h"
#include "util.h"
#include <kj/one-of.h>
#include <kj/compat/gzip.h>
#include <kj/compat/brotli.h>

namespace workerd::api {

// =======================================================================================
// EncodedAsyncInputStream

namespace {

class EncodedAsyncInputStream final: public ReadableStreamSource {
  // A wrapper around a native `kj::AsyncInputStream` which knows the underlying encoding of the
  // stream and whether or not it requires pending event registration.

public:
  explicit EncodedAsyncInputStream(kj::Own<kj::AsyncInputStream> inner, StreamEncoding encoding,
                                   IoContext& context);

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override;
  // Read bytes in identity encoding. If the stream is not already in identity encoding, it will be
  // converted to identity encoding via an appropriate stream wrapper.

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding outEncoding) override;
  // Return the number of bytes, if known, which this input stream will produce if the sink is known
  // to be of a particular encoding.
  //
  // It is likely an error to call this function without immediately following it with a pumpTo()
  // to a EncodedAsyncOutputStream of that exact encoding.

  kj::Maybe<Tee> tryTee(uint64_t limit) override;
  // Consume this stream and return two streams with the same encoding that read the exact same
  // data.
  //
  // This implementation of `tryTee()` is not technically required for correctness, but prevents
  // re-encoding (and converting Content-Length responses to chunk-encoded responses) gzip and
  // brotli streams.

private:
  friend class EncodedAsyncOutputStream;

  void ensureIdentityEncoding();

  kj::Own<kj::AsyncInputStream> inner;
  StreamEncoding encoding;

  IoContext& ioContext;
};

EncodedAsyncInputStream::EncodedAsyncInputStream(
    kj::Own<kj::AsyncInputStream> inner, StreamEncoding encoding, IoContext& context)
    : inner(kj::mv(inner)), encoding(encoding), ioContext(context) {}

kj::Promise<size_t> EncodedAsyncInputStream::tryRead(
    void* buffer, size_t minBytes, size_t maxBytes) {
  ensureIdentityEncoding();

  return kj::evalNow([&]() {
    return inner->tryRead(buffer, minBytes, maxBytes)
      .attach(ioContext.registerPendingEvent());
  }).catch_([](kj::Exception&& exception) -> kj::Promise<size_t> {
    KJ_IF_MAYBE(e, translateKjException(exception, {
      { "gzip compressed stream ended prematurely"_kj,
        "Gzip compressed stream ended prematurely."_kj },
      { "gzip decompression failed"_kj,
        "Gzip decompression failed." },
      { "brotli state allocation failed"_kj,
        "Brotli state allocation failed." },
      { "invalid brotli window size"_kj,
        "Invalid brotli window size." },
      { "invalid brotli compression level"_kj,
        "Invalid brotli compression level." },
      { "brotli window size too big"_kj,
        "Brotli window size too big." },
      { "brotli decompression failed"_kj,
        "Brotli decompression failed." },
      { "brotli compression failed"_kj,
        "Brotli compression failed." },
      { "brotli compressed stream ended prematurely"_kj,
        "Brotli compressed stream ended prematurely." },
    })) {
      return kj::mv(*e);
    }

    // Let the original exception pass through, since it is likely already a jsg.TypeError.
    return kj::mv(exception);
  });
}

kj::Maybe<uint64_t> EncodedAsyncInputStream::tryGetLength(StreamEncoding outEncoding) {
  if (outEncoding == encoding) {
    return inner->tryGetLength();
  } else {
    // We have no idea what the length will be once encoded/decoded.
    return nullptr;
  }
}

kj::Maybe<ReadableStreamSource::Tee> EncodedAsyncInputStream::tryTee(uint64_t limit) {
  // We tee the stream in its original encoding, because chances are highest that we'll be pumped
  // to sinks that are of the same encoding, and only read in identity encoding no more than once.
  //
  // Additionally, we should propagate the fact that this stream is a native stream to the branches
  // of the tee, so that branches which fall behind their siblings (and thus are reading from the
  // tee buffer) still register pending events correctly.
  auto tee = kj::newTee(kj::mv(inner), limit);

  Tee result;
  result.branches[0] = newSystemStream(newTeeErrorAdapter(kj::mv(tee.branches[0])), encoding);
  result.branches[1] = newSystemStream(newTeeErrorAdapter(kj::mv(tee.branches[1])), encoding);
  return kj::mv(result);
}

void EncodedAsyncInputStream::ensureIdentityEncoding() {
  // Decompression gets added to the stream here if needed based on the content encoding.
  if (encoding == StreamEncoding::GZIP) {
    inner = kj::heap<kj::GzipAsyncInputStream>(*inner).attach(kj::mv(inner));
    encoding = StreamEncoding::IDENTITY;
  } else if (encoding == StreamEncoding::BROTLI) {
    inner = kj::heap<kj::BrotliAsyncInputStream>(*inner).attach(kj::mv(inner));
    encoding = StreamEncoding::IDENTITY;
  } else {
    // We currently support gzip and brotli as non-identity content encodings.
    KJ_ASSERT(encoding == StreamEncoding::IDENTITY);
  }
}

// =======================================================================================
// EncodedAsyncOutputStream

class EncodedAsyncOutputStream final: public WritableStreamSink {
  // A wrapper around a native `kj::AsyncOutputStream` which knows the underlying encoding of the
  // stream and optimizes pumps from `EncodedAsyncInputStream`.
  //
  // The inner will be held on to right up until either end() or abort() is called.
  // This is important because some AsyncOutputStream implementations perform cleanup
  // operations equivalent to end() in their destructors (for instance HttpChunkedEntityWriter).
  // If we wait to clear the kj::Own when the EncodedAsyncOutputStream is destroyed, and the
  // EncodedAsyncOutputStream is owned (for instance) by an IoOwn, then the lifetime of the
  // inner may be extended past when it should. Eventually, kj::AsyncOutputStream should
  // probably have a distinct end() method of its own that we can defer to, but until it
  // does, it is important for us to release it as soon as end() or abort() are called.

public:
  explicit EncodedAsyncOutputStream(kj::Own<kj::AsyncOutputStream> inner, StreamEncoding encoding,
                                    IoContext& context);

  kj::Promise<void> write(const void* buffer, size_t size) override;
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override;

  kj::Maybe<kj::Promise<DeferredProxy<void>>> tryPumpFrom(
      ReadableStreamSource& input, bool end) override;

  kj::Promise<void> end() override;

  void abort(kj::Exception reason) override;

private:
  void ensureIdentityEncoding();

  kj::AsyncOutputStream& getInner();
  // Unwrap `inner` as a `kj::AsyncOutputStream`.
  //
  // TODO(cleanup): Obviously this is polymorphism. We should be able to do better.

  struct Ended {
    // A sentinel indicating that the EncodedOutputStream has ended and is no longer usable.
  };

  kj::OneOf<kj::Own<kj::AsyncOutputStream>, kj::Own<kj::GzipAsyncOutputStream>,
            kj::Own<kj::BrotliAsyncOutputStream>, Ended> inner;
  // I use a OneOf here rather than probing with downcasts because end() must be called for
  // correctness rather than for optimization. I "know" this code will never be compiled w/o RTTI,
  // but I'm paranoid.

  StreamEncoding encoding;

  IoContext& ioContext;
};

EncodedAsyncOutputStream::EncodedAsyncOutputStream(
    kj::Own<kj::AsyncOutputStream> inner, StreamEncoding encoding, IoContext& context)
    : inner(kj::mv(inner)), encoding(encoding), ioContext(context) {}

kj::Promise<void> EncodedAsyncOutputStream::write(const void* buffer, size_t size) {
  // Alternatively, we could throw here but this is erring on the side of leniency.
  if (inner.is<Ended>()) return kj::READY_NOW;

  ensureIdentityEncoding();

  return getInner().write(buffer, size)
      .attach(ioContext.registerPendingEvent());
}

kj::Promise<void> EncodedAsyncOutputStream::write(
    kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) {
  // Alternatively, we could throw here but this is erring on the side of leniency.
  if (inner.is<Ended>()) return kj::READY_NOW;

  ensureIdentityEncoding();

  return getInner().write(pieces)
      .attach(ioContext.registerPendingEvent());
}

kj::Maybe<kj::Promise<DeferredProxy<void>>> EncodedAsyncOutputStream::tryPumpFrom(
    ReadableStreamSource& input, bool end) {

  // If this output stream has already been ended, then there's nothing more to
  // pump into it, just return an immediately resolved promise. Alternatively
  // we could throw here.
  if (inner.is<Ended>()) {
    return kj::Promise<DeferredProxy<void>>(DeferredProxy<void> { kj::READY_NOW });
  }

  KJ_IF_MAYBE(nativeInput, kj::dynamicDowncastIfAvailable<EncodedAsyncInputStream>(input)) {
    // We can avoid putting our inner streams into identity encoding if the input and output both
    // have the same encoding. Since ReadableStreamSource/WritableStreamSink always pump everything
    // (there is no `amount` parameter like in the KJ equivalents), we can assume that we will
    // always stop at a valid endpoint.
    //
    // Note that even if we have to pump in identity encoding, there is no reason to return nullptr.
    // We can still optimize the pump a little by registering only a single pending event rather
    // than falling back to the heavier weight algorithm in ReadableStreamSource, which depends on
    // tryRead() and write() registering their own individual events on every call.
    if (nativeInput->encoding != encoding) {
      ensureIdentityEncoding();
      nativeInput->ensureIdentityEncoding();
    }

    auto promise = nativeInput->inner->pumpTo(getInner()).ignoreResult();
    if (end) {
      KJ_IF_MAYBE(gz, inner.tryGet<kj::Own<kj::GzipAsyncOutputStream>>()) {
        promise = promise.then([&gz = *gz]() { return gz->end(); });
      }
      KJ_IF_MAYBE(br, inner.tryGet<kj::Own<kj::BrotliAsyncOutputStream>>()) {
        promise = promise.then([&br = *br]() { return br->end(); });
      }
    }

    // Since this is a system stream, the pump task is eligible to be deferred past IoContext
    // lifetime!
    return kj::Promise<DeferredProxy<void>>(DeferredProxy<void> { kj::mv(promise) });
  }

  return nullptr;
}

kj::Promise<void> EncodedAsyncOutputStream::end() {
  if (inner.is<Ended>()) return kj::READY_NOW;

  kj::Promise<void> promise = kj::READY_NOW;

  KJ_IF_MAYBE(gz, inner.tryGet<kj::Own<kj::GzipAsyncOutputStream>>()) {
    promise = (*gz)->end().attach(kj::mv(*gz));
  }
  KJ_IF_MAYBE(br, inner.tryGet<kj::Own<kj::BrotliAsyncOutputStream>>()) {
    promise = (*br)->end().attach(kj::mv(*br));
  }

  KJ_IF_MAYBE(stream, inner.tryGet<kj::Own<kj::AsyncOutputStream>>()) {
    if (auto casted = dynamic_cast<kj::AsyncIoStream*>(stream->get())) {
      casted->shutdownWrite();
    }
    promise = promise.attach(kj::mv(*stream));
  }

  inner.init<Ended>();

  return promise.attach(ioContext.registerPendingEvent());
}

void EncodedAsyncOutputStream::abort(kj::Exception reason) {
  inner.init<Ended>();
}

void EncodedAsyncOutputStream::ensureIdentityEncoding() {
  // Compression gets added to the stream here if needed based on the content encoding.
  KJ_DASSERT(!inner.is<Ended>(), "the EncodedAsyncOutputStream has been ended or aborted");
  if (encoding == StreamEncoding::GZIP) {
    // This is safe because only a kj::AsyncOutputStream can have non-identity encoding.
    auto& stream = inner.get<kj::Own<kj::AsyncOutputStream>>();

    inner = kj::heap<kj::GzipAsyncOutputStream>(*stream).attach(kj::mv(stream));
    encoding = StreamEncoding::IDENTITY;
  } else if (encoding == StreamEncoding::BROTLI) {
    auto& stream = inner.get<kj::Own<kj::AsyncOutputStream>>();

    inner = kj::heap<kj::BrotliAsyncOutputStream>(*stream).attach(kj::mv(stream));
    encoding = StreamEncoding::IDENTITY;
  } else {
    // We currently support gzip and brotli as non-identity content encodings.
    KJ_ASSERT(encoding == StreamEncoding::IDENTITY);
  }
}

kj::AsyncOutputStream& EncodedAsyncOutputStream::getInner() {
  KJ_SWITCH_ONEOF(inner) {
    KJ_CASE_ONEOF(stream, kj::Own<kj::AsyncOutputStream>) {
      return *stream;
    }
    KJ_CASE_ONEOF(gz, kj::Own<kj::GzipAsyncOutputStream>) {
      return *gz;
    }
    KJ_CASE_ONEOF(br, kj::Own<kj::BrotliAsyncOutputStream>) {
      return *br;
    }
    KJ_CASE_ONEOF(ended, Ended) {
      KJ_FAIL_ASSERT("the EncodedAsyncOutputStream has been ended or aborted.");
    }
  }

  KJ_UNREACHABLE;
}

}  // namespace

kj::Own<ReadableStreamSource> newSystemStream(
    kj::Own<kj::AsyncInputStream> inner, StreamEncoding encoding, IoContext& context) {
  return kj::heap<EncodedAsyncInputStream>(kj::mv(inner), encoding, context);
}
kj::Own<WritableStreamSink> newSystemStream(
    kj::Own<kj::AsyncOutputStream> inner, StreamEncoding encoding, IoContext& context) {
  return kj::heap<EncodedAsyncOutputStream>(kj::mv(inner), encoding, context);
}

SystemMultiStream newSystemMultiStream(
    kj::Own<kj::AsyncIoStream> stream, IoContext& context) {

  auto wrapped = kj::refcountedWrapper(kj::mv(stream));
  return {
    .readable = kj::heap<EncodedAsyncInputStream>(
        wrapped->addWrappedRef(), StreamEncoding::IDENTITY, context),
    .writable = kj::heap<EncodedAsyncOutputStream>(
        wrapped->addWrappedRef(), StreamEncoding::IDENTITY, context)
  };
}

ContentEncodingOptions::ContentEncodingOptions(CompatibilityFlags::Reader flags)
    : brotliEnabled(flags.getBrotliContentEncoding()) {}

StreamEncoding getContentEncoding(IoContext& context, const kj::HttpHeaders& headers,
                                  Response::BodyEncoding bodyEncoding,
                                  ContentEncodingOptions options) {
  if (bodyEncoding == Response::BodyEncoding::MANUAL) {
    return StreamEncoding::IDENTITY;
  }
  KJ_IF_MAYBE(encodingStr, headers.get(context.getHeaderIds().contentEncoding)) {
    if (*encodingStr == "gzip") {
      return StreamEncoding::GZIP;
    } else if (options.brotliEnabled && *encodingStr == "br") {
      return StreamEncoding::BROTLI;
    }
  }
  return StreamEncoding::IDENTITY;
}

}  // namespace workerd::api
