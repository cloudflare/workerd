// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>

#include <unicode/ucnv.h>

namespace workerd::api {

// The encodings listed here are defined as required by the Encoding spec.
// The first label is enum we use to identify the encoding in code, while
// the second label is the public identifier.
#define EW_ENCODINGS(V)                                                                            \
  V(Utf8, "utf-8")                                                                                 \
  V(Ibm866, "ibm-866")                                                                             \
  V(Iso8859_2, "iso-8859-2")                                                                       \
  V(Iso8859_3, "iso-8859-3")                                                                       \
  V(Iso8859_4, "iso-8859-4")                                                                       \
  V(Iso8859_5, "iso-8859-5")                                                                       \
  V(Iso8859_6, "iso-8859-6")                                                                       \
  V(Iso8859_7, "iso-8859-7")                                                                       \
  V(Iso8859_8, "iso-8859-8")                                                                       \
  V(Iso8859_8i, "iso-8859-8-i")                                                                    \
  V(Iso8859_10, "iso-8859-10")                                                                     \
  V(Iso8859_13, "iso-8859-13")                                                                     \
  V(Iso8859_14, "iso-8859-14")                                                                     \
  V(Iso8859_15, "iso-8859-15")                                                                     \
  V(Iso8859_16, "iso-8859-16")                                                                     \
  V(Ko18_r, "koi8-r")                                                                              \
  V(Koi8_u, "koi8-u")                                                                              \
  V(Macintosh, "macintosh")                                                                        \
  V(Windows_874, "windows-874")                                                                    \
  V(Windows_1250, "windows-1250")                                                                  \
  V(Windows_1251, "windows-1251")                                                                  \
  V(Windows_1252, "windows-1252")                                                                  \
  V(Windows_1253, "windows-1253")                                                                  \
  V(Windows_1254, "windows-1254")                                                                  \
  V(Windows_1255, "windows-1255")                                                                  \
  V(Windows_1256, "windows-1256")                                                                  \
  V(Windows_1257, "windows-1257")                                                                  \
  V(Windows_1258, "windows-1258")                                                                  \
  V(X_Mac_Cyrillic, "x-mac-cyrillic")                                                              \
  V(Gbk, "gbk")                                                                                    \
  V(Gb18030, "gb18030")                                                                            \
  V(Big5, "big5")                                                                                  \
  V(Euc_Jp, "euc-jp")                                                                              \
  V(Iso2022_Jp, "iso-2022-jp")                                                                     \
  V(Shift_Jis, "shift-jis")                                                                        \
  V(Euc_Kr, "euc-kr")                                                                              \
  V(Replacement, "replacement")                                                                    \
  V(Utf16be, "utf-16be")                                                                           \
  V(Utf16le, "utf-16le")                                                                           \
  V(X_User_Defined, "x-user-defined")

enum class Encoding {
  INVALID,
#define V(name, _) name,
  EW_ENCODINGS(V)
#undef V
};

// A Decoder provides the underlying implementation of a TextDecoder.
class Decoder {
public:
  virtual ~Decoder() noexcept(true) {}
  virtual Encoding getEncoding() = 0;
  virtual kj::Maybe<jsg::JsString> decode(
      jsg::Lock& js, kj::ArrayPtr<const kj::byte> buffer, bool flush = false) = 0;

  virtual void reset() {}
};

// Decoder implementation that provides a fast-track for US-ASCII.
class AsciiDecoder final: public Decoder {
public:
  AsciiDecoder() = default;
  AsciiDecoder(AsciiDecoder&&) = default;
  AsciiDecoder& operator=(AsciiDecoder&&) = default;
  KJ_DISALLOW_COPY(AsciiDecoder);

  Encoding getEncoding() override {
    return Encoding::Windows_1252;
  }

  kj::Maybe<jsg::JsString> decode(
      jsg::Lock& js, kj::ArrayPtr<const kj::byte> buffer, bool flush = false) override;
};

// Decoder implementation that uses ICU's built-in conversion APIs.
// ICU's decoder is fairly comprehensive, covering the full range
// of encodings required by the Encoding specification.
class IcuDecoder final: public Decoder {
public:
  IcuDecoder(Encoding encoding, UConverter* converter, bool ignoreBom)
      : encoding(encoding),
        inner(converter),
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

  bool ignoreBom;
  bool bomSeen;
};

// Implements the TextDecoder interface as prescribed by:
// https://encoding.spec.whatwg.org/#interface-textdecoder
class TextDecoder final: public jsg::Object {
public:
  using DecoderImpl = kj::OneOf<AsciiDecoder, IcuDecoder>;

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
      jsg::Optional<kj::String> label, jsg::Optional<ConstructorOptions> options);

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

  static jsg::Ref<TextEncoder> constructor();

  jsg::BufferSource encode(jsg::Lock& js, jsg::Optional<jsg::JsString> input);

  EncodeIntoResult encodeInto(jsg::Lock& js, jsg::JsString input, jsg::BufferSource buffer);

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
    });
  }
};

#define EW_ENCODING_ISOLATE_TYPES                                                                  \
  api::TextDecoder, api::TextEncoder, api::TextDecoder::ConstructorOptions,                        \
      api::TextDecoder::DecodeOptions, api::TextEncoder::EncodeIntoResult
}  // namespace workerd::api
