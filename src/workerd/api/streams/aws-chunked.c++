// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "aws-chunked.h"

#include "sync-transform.h"

#include <workerd/api/headers.h>
#include <workerd/io/io-context.h>

#include <kj/vector.h>

#include <cmath>

namespace workerd::api {

namespace {

constexpr size_t DEFAULT_MAX_TRAILER_BYTES = 16 * 1024;
// A size line's extensions hold at most a chunk signature in practice; this bounds what is
// buffered while skipping them.
constexpr size_t MAX_EXTENSION_BYTES = 4096;
// 16 hex digits express any 64-bit size.
constexpr size_t MAX_SIZE_DIGITS = 16;

using Trailers = kj::Array<kj::Tuple<kj::String, kj::String>>;

kj::Maybe<uint8_t> hexDigitValue(kj::byte c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return kj::none;
}

bool isTokenChar(kj::byte c) {
  // RFC 9110 tchar.
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      kj::StringPtr("!#$%&'*+-.^_`|~").findFirst(static_cast<char>(c)) != kj::none;
}

kj::ArrayPtr<const kj::byte> trimWhitespace(kj::ArrayPtr<const kj::byte> text) {
  while (text.size() > 0 && (text.front() == ' ' || text.front() == '\t')) text = text.slice(1);
  while (text.size() > 0 && (text.back() == ' ' || text.back() == '\t')) {
    text = text.first(text.size() - 1);
  }
  return text;
}

class AwsChunkedDecodeStage final: public SyncTransformStage {
 public:
  AwsChunkedDecodeStage(kj::Maybe<uint64_t> decodedLength,
      kj::Maybe<kj::Array<kj::String>> allowedTrailers,
      size_t maxTrailerBytes,
      bool drainOnError,
      kj::Own<kj::PromiseFulfiller<Trailers>> trailersFulfiller)
      : decodedLength(decodedLength),
        allowedTrailers(kj::mv(allowedTrailers)),
        maxTrailerBytes(maxTrailerBytes),
        drainOnError(drainOnError),
        trailersFulfiller(kj::mv(trailersFulfiller)) {}

  ~AwsChunkedDecodeStage() noexcept(false) {
    trailersFulfiller->reject(
        JSG_KJ_EXCEPTION(DISCONNECTED, TypeError, "The aws-chunked stream did not finish."));
  }

  void push(kj::ArrayPtr<const kj::byte> input, Output& output) override {
    if (error != kj::none) {
      // Draining: the input is read and discarded until it ends.
      return;
    }
    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() { decode(input, output); })) {
      fail(kj::mv(exception));
    }
  }

  void end(Output& output) override {
    if (error == kj::none && state != State::DONE) {
      fail(malformed("The aws-chunked body ended before its final chunk and trailer."));
    }
    KJ_IF_SOME(exception, error) {
      kj::throwFatalException(exception.clone());
    }
    trailersFulfiller->fulfill(kj::mv(KJ_ASSERT_NONNULL(trailers)));
  }

  void cancel(const kj::Exception& reason) override {
    trailersFulfiller->reject(reason.clone());
  }

  kj::Maybe<uint64_t> getOutputLength() const override {
    return decodedLength;
  }

 private:
  enum class State {
    SIZE,
    EXTENSION,
    SIZE_LF,
    DATA,
    DATA_CR,
    DATA_LF,
    TRAILER,
    DONE,
  };

  kj::Maybe<uint64_t> decodedLength;
  kj::Maybe<kj::Array<kj::String>> allowedTrailers;
  size_t maxTrailerBytes;
  bool drainOnError;
  kj::Own<kj::PromiseFulfiller<Trailers>> trailersFulfiller;

  State state = State::SIZE;
  size_t sizeDigits = 0;
  size_t extensionBytes = 0;
  uint64_t chunkRemaining = 0;
  uint64_t decoded = 0;
  kj::Vector<kj::byte> trailerBytes;
  kj::Maybe<Trailers> trailers;
  kj::Maybe<kj::Exception> error;

  static kj::Exception malformed(kj::StringPtr message) {
    return JSG_KJ_EXCEPTION(FAILED, TypeError, message);
  }

  // Records the stream's failure. Unless draining, it is thrown at once and errors both sides;
  // otherwise it is thrown by end().
  void fail(kj::Exception exception) {
    trailersFulfiller->reject(exception.clone());
    if (drainOnError) {
      error = kj::mv(exception);
    } else {
      error = exception.clone();
      kj::throwFatalException(kj::mv(exception));
    }
  }

  void decode(kj::ArrayPtr<const kj::byte> input, Output& output) {
    while (input.size() > 0) {
      switch (state) {
        case State::SIZE: {
          kj::byte c = input.front();
          KJ_IF_SOME(digit, hexDigitValue(c)) {
            JSG_REQUIRE(sizeDigits < MAX_SIZE_DIGITS, TypeError,
                "An aws-chunked chunk size has more than ", MAX_SIZE_DIGITS, " digits.");
            chunkRemaining = chunkRemaining << 4 | digit;
            sizeDigits++;
          } else {
            JSG_REQUIRE(sizeDigits > 0 && (c == ';' || c == '\r'), TypeError,
                "Malformed aws-chunked chunk size.");
            state = c == ';' ? State::EXTENSION : State::SIZE_LF;
          }
          input = input.slice(1);
          break;
        }
        case State::EXTENSION: {
          auto end = input.findFirst('\r').orDefault(input.size());
          extensionBytes += end;
          JSG_REQUIRE(extensionBytes <= MAX_EXTENSION_BYTES, TypeError,
              "An aws-chunked chunk extension is longer than ", MAX_EXTENSION_BYTES, " bytes.");
          if (end < input.size()) {
            state = State::SIZE_LF;
            end++;
          }
          input = input.slice(end);
          break;
        }
        case State::SIZE_LF:
          JSG_REQUIRE(input.front() == '\n', TypeError, "Malformed aws-chunked chunk size line.");
          input = input.slice(1);
          startChunk();
          break;
        case State::DATA: {
          size_t n = kj::min(chunkRemaining, static_cast<uint64_t>(input.size()));
          decoded += n;
          output.add(kj::heapArray(input.first(n)));
          chunkRemaining -= n;
          input = input.slice(n);
          if (chunkRemaining == 0) state = State::DATA_CR;
          break;
        }
        case State::DATA_CR:
          JSG_REQUIRE(input.front() == '\r', TypeError,
              "An aws-chunked chunk is longer than its declared size.");
          input = input.slice(1);
          state = State::DATA_LF;
          break;
        case State::DATA_LF:
          JSG_REQUIRE(input.front() == '\n', TypeError, "Malformed aws-chunked chunk ending.");
          input = input.slice(1);
          state = State::SIZE;
          break;
        case State::TRAILER:
          input = consumeTrailer(input);
          break;
        case State::DONE:
          JSG_FAIL_REQUIRE(TypeError, "The aws-chunked body continues past its trailer.");
      }
    }
  }

  void startChunk() {
    sizeDigits = 0;
    extensionBytes = 0;
    if (chunkRemaining == 0) {
      // The last chunk. Its trailer section follows.
      KJ_IF_SOME(expected, decodedLength) {
        JSG_REQUIRE(decoded == expected, TypeError, "The aws-chunked body decoded to ", decoded,
            " bytes, but its decoded length is ", expected, ".");
      }
      state = State::TRAILER;
    } else {
      KJ_IF_SOME(expected, decodedLength) {
        JSG_REQUIRE(chunkRemaining <= expected - decoded, TypeError,
            "The aws-chunked body decodes to more than its decoded length (", expected, " bytes).");
      }
      state = State::DATA;
    }
  }

  // Buffers the trailer section until the empty line that ends it, then parses it. Returns the
  // input left after the section.
  kj::ArrayPtr<const kj::byte> consumeTrailer(kj::ArrayPtr<const kj::byte> input) {
    size_t before = trailerBytes.size();
    size_t take = kj::min(input.size(), maxTrailerBytes + 1 - before);
    trailerBytes.addAll(input.first(take));
    // The section ends at the first empty line: either it starts with one, or a line ending is
    // followed by one. Only the bytes just added, and the three before them, can complete it.
    auto bytes = trailerBytes.asPtr();
    kj::Maybe<size_t> sectionEnd;
    if (bytes.size() >= 2 && bytes[0] == '\r' && bytes[1] == '\n') {
      sectionEnd = 2;
    } else {
      for (size_t i = before < 3 ? 0 : before - 3; i + 4 <= bytes.size(); i++) {
        if (bytes[i] == '\r' && bytes[i + 1] == '\n' && bytes[i + 2] == '\r' &&
            bytes[i + 3] == '\n') {
          sectionEnd = i + 4;
          break;
        }
      }
    }
    KJ_IF_SOME(end, sectionEnd) {
      JSG_REQUIRE(end <= maxTrailerBytes, TypeError, "The aws-chunked trailer is longer than ",
          maxTrailerBytes, " bytes.");
      trailers = parseTrailer(bytes.first(end - 2));
      trailerBytes.clear();
      state = State::DONE;
      return input.slice(end - before);
    }
    JSG_REQUIRE(bytes.size() <= maxTrailerBytes, TypeError,
        "The aws-chunked trailer is longer than ", maxTrailerBytes, " bytes.");
    return input.slice(take);
  }

  // Parses `name: value` lines, each ending in CRLF.
  Trailers parseTrailer(kj::ArrayPtr<const kj::byte> section) {
    kj::Vector<kj::Tuple<kj::String, kj::String>> result;
    while (section.size() > 0) {
      size_t lineEnd = 0;
      while (!(section[lineEnd] == '\r' && section[lineEnd + 1] == '\n')) lineEnd++;
      auto line = section.first(lineEnd);
      section = section.slice(lineEnd + 2);

      auto colon = JSG_REQUIRE_NONNULL(
          line.findFirst(':'), TypeError, "Malformed aws-chunked trailer field.");
      auto name = line.first(colon);
      JSG_REQUIRE(name.size() > 0, TypeError, "Malformed aws-chunked trailer field name.");
      for (auto c: name) {
        JSG_REQUIRE(isTokenChar(c), TypeError, "Malformed aws-chunked trailer field name.");
      }
      auto value = trimWhitespace(line.slice(colon + 1));
      for (auto c: value) {
        JSG_REQUIRE(c == '\t' || (c >= 0x20 && c != 0x7f), TypeError,
            "Malformed aws-chunked trailer field value.");
      }
      auto nameString = kj::str(name.asChars());
      if (isAllowed(nameString)) {
        result.add(kj::tuple(kj::mv(nameString), kj::str(value.asChars())));
      }
    }
    return result.releaseAsArray();
  }

  bool isAllowed(kj::StringPtr name) const {
    KJ_IF_SOME(allowed, allowedTrailers) {
      for (auto& entry: allowed) {
        if (strcaseeq(entry, name)) return true;
      }
      return false;
    }
    return true;
  }

  static bool strcaseeq(kj::StringPtr a, kj::StringPtr b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
      char x = a[i], y = b[i];
      if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
      if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
      if (x != y) return false;
    }
    return true;
  }
};

kj::Maybe<uint64_t> toOptionalLength(jsg::Optional<double> value, kj::StringPtr name) {
  return value.map([&](double v) -> uint64_t {
    JSG_REQUIRE(v >= 0 && v <= 9007199254740991.0 && std::floor(v) == v, RangeError, name,
        " must be a non-negative integer no greater than Number.MAX_SAFE_INTEGER.");
    return static_cast<uint64_t>(v);
  });
}

}  // namespace

jsg::Ref<AwsChunkedDecoderStream> AwsChunkedDecoderStream::constructor(
    jsg::Lock& js, jsg::Optional<Options> maybeOptions) {
  auto options = kj::mv(maybeOptions).orDefault({});
  auto decodedLength = toOptionalLength(options.decodedLength, "decodedLength");
  size_t maxTrailerBytes = toOptionalLength(options.maxTrailerBytes, "maxTrailerBytes")
                               .orDefault(DEFAULT_MAX_TRAILER_BYTES);

  auto& ioContext = IoContext::current();
  auto paf = kj::newPromiseAndFulfiller<Trailers>();
  auto stage = kj::heap<AwsChunkedDecodeStage>(decodedLength, kj::mv(options.allowedTrailers),
      maxTrailerBytes, options.drainOnError.orDefault(true), kj::mv(paf.fulfiller));
  // Up to one network read of decoded data may wait to be read.
  auto streams = newSyncTransformStreams(js, ioContext, kj::mv(stage), 64 * 1024);

  auto trailers = ioContext.awaitIo(js, kj::mv(paf.promise), [](jsg::Lock& js, Trailers fields) {
    auto headers = js.alloc<Headers>();
    for (auto& field: fields) {
      headers->append(js, kj::mv(kj::get<0>(field)), kj::mv(kj::get<1>(field)));
    }
    return headers;
  });
  // A caller that only reads the body need not wait for the trailers.
  trailers.markAsHandled(js);

  return js.alloc<AwsChunkedDecoderStream>(
      kj::mv(streams.readable), kj::mv(streams.writable), kj::mv(trailers));
}

}  // namespace workerd::api
