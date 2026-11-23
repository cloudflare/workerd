// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "mimetype-legacy.h"

#include <workerd/util/autogate.h>

#include <kj/encoding.h>
#include <kj/test.h>
#include <kj/vector.h>

namespace workerd {
namespace {

// The Rust parser and serializer must agree with the C++ ones byte for byte. With the
// RUST_MIMETYPE autogate on, mimetype.h is Rust and legacy:: is C++.

kj::String describe(kj::ArrayPtr<const char> input) {
  return kj::str('"', kj::encodeCEscape(input), '"');
}

void expectSameMimeType(kj::StringPtr what,
    kj::ArrayPtr<const char> input,
    kj::Maybe<MimeType>& rust,
    kj::Maybe<MimeType>& cpp) {
  KJ_IF_SOME(r, rust) {
    auto& c = KJ_ASSERT_NONNULL(cpp, "only Rust accepted", what, describe(input), r);
    KJ_ASSERT(r.type() == c.type(), what, describe(input), r, c);
    KJ_ASSERT(r.subtype() == c.subtype(), what, describe(input), r, c);
    KJ_ASSERT(r.params().size() == c.params().size(), what, describe(input), r, c);
    auto ri = r.params().begin();
    for (auto& entry: c.params()) {
      KJ_ASSERT(ri->key == entry.key, what, describe(input), r, c);
      KJ_ASSERT(ri->value == entry.value, what, describe(input), r, c);
      ++ri;
    }
    auto serialized = legacy::mimeTypeToString(c);
    KJ_ASSERT(r.toString() == serialized, what, describe(input), serialized);
    KJ_ASSERT(r.paramsToString() == legacy::mimeTypeParamsToString(c), what, describe(input));
  } else {
    KJ_IF_SOME(c, cpp) {
      KJ_FAIL_ASSERT("only C++ accepted", what, describe(input), legacy::mimeTypeToString(c));
    }
  }
}

// Serializes a MimeType with parameters that parsing would never produce, such as values with
// bytes that are not allowed, with both serializers.
void expectSameSerialization(kj::ArrayPtr<const char> value) {
  MimeType::MimeParams params;
  params.insert(kj::str("v"), kj::str(value));
  params.insert(kj::str("boundary"), kj::str(value));
  params.insert(kj::str("e"), kj::str());
  MimeType mimeType(kj::str("type"), kj::str("subtype"), kj::mv(params));
  KJ_ASSERT(mimeType.toString() == legacy::mimeTypeToString(mimeType), describe(value));
  KJ_ASSERT(mimeType.paramsToString() == legacy::mimeTypeParamsToString(mimeType), describe(value));
}

void expectSameValidation(kj::ArrayPtr<const char> input) {
  auto text = kj::str(input);
  MimeType mimeType("type"_kj, "subtype"_kj);
  KJ_ASSERT(mimeType.setType(text) == legacy::isMimeToken(input), describe(input));
  KJ_ASSERT(mimeType.setSubtype(text) == legacy::isMimeToken(input), describe(input));
  KJ_ASSERT(mimeType.addParam(input, "value"_kj) == legacy::isMimeToken(input), describe(input));
  KJ_ASSERT(
      mimeType.addParam("name"_kj, input) == legacy::isMimeParamValue(input), describe(input));
}

void expectSameExtract(kj::ArrayPtr<const char> input) {
  // A kj::String may hold a NUL, which extract() reads as any other byte.
  auto text = kj::str(input);
  auto rust = MimeType::extract(text);
  auto cpp = legacy::extractMimeType(text);
  expectSameMimeType("extract"_kj, input, rust, cpp);
}

void expectSame(kj::ArrayPtr<const char> input) {
  for (auto options: {MimeType::DEFAULT, MimeType::IGNORE_PARAMS}) {
    auto rust = MimeType::tryParse(input, options);
    auto cpp = legacy::tryParseMimeType(input, options);
    expectSameMimeType(options == MimeType::DEFAULT ? "tryParse"_kj : "tryParse IGNORE_PARAMS"_kj,
        input, rust, cpp);
  }
  expectSameExtract(input);
}

// Pieces that MIME type syntax gives a meaning to, and bytes it treats specially.
const kj::StringPtr PIECES[] = {
  "a"_kj,
  "B"_kj,
  "/"_kj,
  ";"_kj,
  "="_kj,
  "\""_kj,
  "\\"_kj,
  ","_kj,
  " "_kj,
  "\t"_kj,
  "\r"_kj,
  "\n"_kj,
  "\x0c"_kj,
  "*"_kj,
  "@"_kj,
  "\x01"_kj,
  "\x7f"_kj,
  "\x80"_kj,
  "\xfe"_kj,
  "\xff"_kj,
  "\xc3\xa9"_kj,
  kj::StringPtr("\0", 1),
  "charset"_kj,
  "ChArSeT"_kj,
  "boundary"_kj,
  "text"_kj,
  "html"_kj,
  "+xml"_kj,
  "utf-8"_kj,
};

// Starts that make the pieces after them land in a type, a subtype, a parameter name, a
// parameter value, a quoted string, or a later part of a header value.
const kj::StringPtr PREFIXES[] = {
  ""_kj,
  "text/"_kj,
  "text/html"_kj,
  "text/html;"_kj,
  "text/html;charset="_kj,
  "text/html;charset=\""_kj,
  "text/html;charset=gbk,"_kj,
  "text/html;charset=gbk, TEXT/HTML"_kj,
  "text/html;charset=\"a,b\";x=y,text/html,"_kj,
  "*/*;charset=gbk,"_kj,
};

KJ_TEST("the Rust MIME type parser agrees with the C++ one on short inputs") {
  util::Autogate::initAutogateForTest({util::AutogateKey::RUST_MIMETYPE});
  KJ_ASSERT(util::Autogate::isEnabled(util::AutogateKey::RUST_MIMETYPE));
  KJ_DEFER(util::Autogate::deinitAutogate());

  constexpr size_t n = kj::size(PIECES);
  size_t cases = 0;
  for (auto i: kj::indices(PREFIXES)) {
    auto prefix = PREFIXES[i];
    // Every sequence of up to three pieces after the first prefixes, and of up to two after the
    // others.
    size_t maxLength = i < 3 ? 3 : 2;
    for (size_t length = 0; length <= maxLength; ++length) {
      size_t count = 1;
      for (size_t i = 0; i < length; ++i) count *= n;
      for (size_t index = 0; index < count; ++index) {
        kj::Vector<char> input;
        input.addAll(prefix);
        for (size_t i = 0, rest = index; i < length; ++i, rest /= n) {
          input.addAll(PIECES[rest % n]);
        }
        expectSame(input);
        ++cases;
      }
    }
  }
  KJ_LOG(INFO, "compared short inputs", cases);
}

KJ_TEST("the Rust MIME type parser agrees with the C++ one on random inputs") {
  util::Autogate::initAutogateForTest({util::AutogateKey::RUST_MIMETYPE});
  KJ_ASSERT(util::Autogate::isEnabled(util::AutogateKey::RUST_MIMETYPE));
  KJ_DEFER(util::Autogate::deinitAutogate());

  // xorshift64*, with a fixed seed so that a failure reproduces.
  uint64_t state = 0x9e3779b97f4a7c15;
  auto next = [&state](uint64_t bound) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return (state * 0x2545f4914f6cdd1d) % bound;
  };

  constexpr size_t cases = 50000;
  for (size_t c = 0; c < cases; ++c) {
    kj::Vector<char> input;
    input.addAll(PREFIXES[next(kj::size(PREFIXES))]);
    auto pieces = next(24);
    for (size_t i = 0; i < pieces; ++i) {
      if (next(4) == 0) {
        // Any byte at all.
        input.add(static_cast<char>(next(256)));
      } else {
        input.addAll(PIECES[next(kj::size(PIECES))]);
      }
    }
    expectSame(input);
  }
}

// Parts of a header value that extract() combines: essences that are equal once lowercased, a
// different one, the */* it skips and ones that are not MIME types, each with a charset in
// either case, quoted, invalid or empty, or with a comma inside quotes.
const kj::StringPtr EXTRACT_ESSENCES[] = {
  "text/html"_kj,
  "TEXT/HTML"_kj,
  "text/plain"_kj,
  "*/*"_kj,
  "bogus"_kj,
  ""_kj,
};
const kj::StringPtr EXTRACT_PARAMS[] = {
  ""_kj,
  ";charset=a"_kj,
  ";ChArSeT=\"b\""_kj,
  ";charset=\xff"_kj,
  ";charset="_kj,
  ";x=\"a,b\""_kj,
};
const kj::StringPtr EXTRACT_SEPARATORS[] = {","_kj, ", "_kj};

KJ_TEST("the Rust MIME type extraction agrees with the C++ one on combined header values") {
  util::Autogate::initAutogateForTest({util::AutogateKey::RUST_MIMETYPE});
  KJ_ASSERT(util::Autogate::isEnabled(util::AutogateKey::RUST_MIMETYPE));
  KJ_DEFER(util::Autogate::deinitAutogate());

  kj::Vector<kj::String> parts;
  for (auto essence: EXTRACT_ESSENCES) {
    for (auto params: EXTRACT_PARAMS) {
      parts.add(kj::str(essence, params));
    }
  }

  constexpr size_t separators = kj::size(EXTRACT_SEPARATORS);

  // Every sequence of one to three parts, with every choice of separator between them.
  size_t cases = 0;
  for (size_t count = 1; count <= 3; ++count) {
    size_t combinations = 1;
    for (size_t i = 0; i < count; ++i) combinations *= parts.size();
    for (size_t i = 1; i < count; ++i) combinations *= separators;
    for (size_t index = 0; index < combinations; ++index) {
      kj::Vector<char> input;
      size_t rest = index;
      for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
          input.addAll(EXTRACT_SEPARATORS[rest % separators]);
          rest /= separators;
        }
        input.addAll(parts[rest % parts.size()]);
        rest /= parts.size();
      }
      expectSameExtract(input);
      ++cases;
    }
  }
  KJ_ASSERT(cases == 189252, cases);
}

KJ_TEST("the Rust MIME type validators and serializer agree with the C++ ones") {
  util::Autogate::initAutogateForTest({util::AutogateKey::RUST_MIMETYPE});
  KJ_ASSERT(util::Autogate::isEnabled(util::AutogateKey::RUST_MIMETYPE));
  KJ_DEFER(util::Autogate::deinitAutogate());

  // Every byte alone, at either end of a token, and between two token bytes.
  for (unsigned b = 0; b < 256; ++b) {
    char c = static_cast<char>(b);
    for (auto& input:
        {kj::heapString(&c, 1), kj::str(c, "ab"), kj::str("ab", c), kj::str("a", c, "b")}) {
      expectSameValidation(input);
      expectSameSerialization(input);
    }
  }
  expectSameValidation(""_kj);
  expectSameSerialization(""_kj);
  for (auto piece: PIECES) {
    for (auto other: PIECES) {
      auto input = kj::str(piece, other);
      expectSameValidation(input);
      expectSameSerialization(input);
    }
  }

  // Many parameters, beyond what the serializer keeps on the stack.
  MimeType::MimeParams params;
  for (unsigned i = 0; i < 40; ++i) {
    params.insert(kj::str("p", i), i % 3 == 0 ? kj::str("needs \"quotes\" ", i) : kj::str(i));
  }
  MimeType many(kj::str("multipart"), kj::str("mixed"), kj::mv(params));
  KJ_ASSERT(many.toString() == legacy::mimeTypeToString(many));
  KJ_ASSERT(many.paramsToString() == legacy::mimeTypeParamsToString(many));

  // The constants, which are parsed while the program starts.
  KJ_ASSERT(MimeType::PLAINTEXT.toString() == MimeType::PLAINTEXT_STRING);
  KJ_ASSERT(MimeType::PLAINTEXT_ASCII.toString() == MimeType::PLAINTEXT_ASCII_STRING);
}

}  // namespace
}  // namespace workerd
