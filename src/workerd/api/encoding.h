// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "encoding-legacy.h"
#include "encoding-shared.h"

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>

#include <unicode/ucnv.h>

namespace workerd::api {

// Decoder implementation that uses ICU's built-in conversion APIs.
// ICU's decoder is fairly comprehensive, covering the full range
// of encodings required by the Encoding specification.
class IcuDecoder final: public Decoder {
 public:
  IcuDecoder(Encoding encoding, UConverter* converter, bool fatal, bool ignoreBom)
      : encoding(encoding),
        inner(converter),
        fatal(fatal),
        ignoreBom(ignoreBom),
        bomSeen(false) {}
  IcuDecoder(IcuDecoder&&) = default;
  IcuDecoder& operator=(IcuDecoder&&) = default;

  static kj::Maybe<IcuDecoder> create(Encoding encoding, bool fatal, bool ignoreBom);

  Encoding getEncoding() override {
    return encoding;
  }

  kj::Maybe<jsg::JsString> decode(
      jsg::Lock& js, kj::ArrayPtr<const kj::byte> buffer, bool flush = false) override;

  void reset() override;

 private:
  struct ConverterDeleter {
    void operator()(UConverter* pointer) const {
      ucnv_close(pointer);
    }
  };

  Encoding encoding;
  std::unique_ptr<UConverter, ConverterDeleter> inner;

  bool fatal;
  bool ignoreBom;
  bool bomSeen;
};

// Implements the TextDecoder interface as prescribed by:
// https://encoding.spec.whatwg.org/#interface-textdecoder
class TextDecoder final: public jsg::Object {
 public:
  using DecoderImpl = kj::OneOf<LegacyDecoder, IcuDecoder>;

  struct ConstructorOptions {
    bool fatal = false;
    bool ignoreBOM = false;

    JSG_STRUCT(fatal, ignoreBOM);
  };

  struct DecodeOptions {
    bool stream = false;

    JSG_STRUCT(stream);
  };

  static jsg::Ref<TextDecoder> constructor(
      jsg::Lock& js, jsg::Optional<kj::String> label, jsg::Optional<ConstructorOptions> options);

  jsg::JsString decode(jsg::Lock& js,
      jsg::Optional<kj::Array<const kj::byte>> input,
      jsg::Optional<DecodeOptions> options);

  kj::StringPtr getEncoding();

  bool getFatal() {
    return ctorOptions.fatal;
  }
  bool getIgnoreBom() {
    return ctorOptions.ignoreBOM;
  }

  JSG_RESOURCE_TYPE(TextDecoder, CompatibilityFlags::Reader flags) {
    JSG_METHOD(decode);
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(encoding, getEncoding);
      JSG_READONLY_PROTOTYPE_PROPERTY(fatal, getFatal);
      JSG_READONLY_PROTOTYPE_PROPERTY(ignoreBOM, getIgnoreBom);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(encoding, getEncoding);
      JSG_READONLY_INSTANCE_PROPERTY(fatal, getFatal);
      JSG_READONLY_INSTANCE_PROPERTY(ignoreBOM, getIgnoreBom);
    }
    // TODO(soon): Defining the constructor override here *should not* be
    // necessary but for some reason the type generation is creating an
    // invalid result without it.
    JSG_TS_OVERRIDE({
      constructor(label?: string, options?: TextDecoderConstructorOptions);
    });
  }

  explicit TextDecoder(DecoderImpl decoder): decoder(kj::mv(decoder)) {}

  explicit TextDecoder(DecoderImpl decoder, const ConstructorOptions& options)
      : decoder(kj::mv(decoder)),
        ctorOptions(options) {}

  kj::Maybe<jsg::JsString> decodePtr(
      jsg::Lock& js, kj::ArrayPtr<const kj::byte> buffer, bool flush);

 private:
  Decoder& getImpl();

  DecoderImpl decoder;
  ConstructorOptions ctorOptions;

  static const DecodeOptions DEFAULT_OPTIONS;
  static constexpr kj::byte DUMMY = 0;
  static const kj::Array<const kj::byte> EMPTY;
};

// Implements the TextEncoder interface as prescribed by:
// https://encoding.spec.whatwg.org/#interface-textencoder
class TextEncoder final: public jsg::Object {
 public:
  struct EncodeIntoResult {
    int read;
    int written;
    // TODO(conform): Perhaps use unsigned long long.

    JSG_STRUCT(read, written);
  };

  static jsg::Ref<TextEncoder> constructor(jsg::Lock& js);

  jsg::BufferSource encode(jsg::Lock& js, jsg::Optional<jsg::JsString> input);

  EncodeIntoResult encodeInto(jsg::Lock& js, jsg::JsString input, jsg::JsUint8Array buffer);

  // UTF-8 is the only encoding type supported by the WHATWG spec.
  kj::StringPtr getEncoding() {
    return "utf-8";
  }

  JSG_RESOURCE_TYPE(TextEncoder, CompatibilityFlags::Reader flags) {
    JSG_METHOD(encode);
    JSG_METHOD(encodeInto);
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(encoding, getEncoding);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(encoding, getEncoding);
    }

    // `encode()` returns `jsg::BufferSource`, which may be an `ArrayBuffer` or `ArrayBufferView`,
    // but the implementation uses `jsg::BufferSource::tryAlloc()` which always tries to allocate a
    // `Uint8Array`. The spec defines that this function returns a `Uint8Array` too.
    JSG_TS_OVERRIDE({
      encode(input?: string): Uint8Array;
      encodeInto(input: string, buffer: Uint8Array): TextEncoderEncodeIntoResult;
    });
  }
};

#define EW_ENCODING_ISOLATE_TYPES                                                                  \
  api::TextDecoder, api::TextEncoder, api::TextDecoder::ConstructorOptions,                        \
      api::TextDecoder::DecodeOptions, api::TextEncoder::EncodeIntoResult
}  // namespace workerd::api
