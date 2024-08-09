#include "data-url.h"
#include <workerd/jsg/url.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

KJ_TEST("DataUrl Basics") {
  auto dataUrl =
      KJ_ASSERT_NONNULL(DataUrl::tryParse("data:text/plain;base64,SGVsbG8sIFdvcmxkIQ=="_kj));
  KJ_ASSERT(dataUrl.getMimeType() == MimeType::PLAINTEXT);
  KJ_ASSERT(dataUrl.getData().asChars() == "Hello, World!"_kj);
}

KJ_TEST("DataUrl Leading/Trailing Whitespace") {
  auto dataUrl = KJ_ASSERT_NONNULL(
      DataUrl::tryParse("    data: \t text/plain \t;base64\t\t ,SGVsbG8sIFdvcmxkIQ==    "_kj));
  KJ_ASSERT(dataUrl.getMimeType() == MimeType::PLAINTEXT);
  KJ_ASSERT(dataUrl.getData().asChars() == "Hello, World!"_kj);
}

KJ_TEST("DataUrl base64 case-insensitive") {
  auto dataUrl = KJ_ASSERT_NONNULL(
      DataUrl::tryParse("    data: \t text/plain \t;BasE64\t\t ,SGVsbG8sIFdvcmxkIQ==    "_kj));
  KJ_ASSERT(dataUrl.getMimeType() == MimeType::PLAINTEXT);
  KJ_ASSERT(dataUrl.getData().asChars() == "Hello, World!"_kj);
}

KJ_TEST("DataUrl no-base64") {
  auto dataUrl = KJ_ASSERT_NONNULL(
      DataUrl::tryParse("    data: \t text/plain \t;a=b\t\t ,SGVsbG8sIFdvcmxkIQ==    "_kj));
  KJ_ASSERT(dataUrl.getMimeType() == MimeType::PLAINTEXT);

  auto& val = KJ_REQUIRE_NONNULL(dataUrl.getMimeType().params().find("a"_kj));
  KJ_ASSERT(val == "b"_kj);

  KJ_ASSERT(dataUrl.getData().asChars() == "SGVsbG8sIFdvcmxkIQ=="_kj);
}

KJ_TEST("DataUrl default mime type") {
  auto dataUrl = KJ_ASSERT_NONNULL(DataUrl::tryParse("data:,Hello, World!"_kj));
  KJ_ASSERT(dataUrl.getMimeType() == MimeType::PLAINTEXT);
  KJ_ASSERT(dataUrl.getData().asChars() == "Hello, World!"_kj);
}

KJ_TEST("DataUrl default mime type") {
  auto dataUrl = KJ_ASSERT_NONNULL(DataUrl::tryParse("data:;,Hello, World!"_kj));
  KJ_ASSERT(dataUrl.getMimeType() == MimeType::PLAINTEXT);
  KJ_ASSERT(dataUrl.getData().asChars() == "Hello, World!"_kj);
}

KJ_TEST("DataUrl default mime type") {
  auto dataUrl = KJ_ASSERT_NONNULL(DataUrl::tryParse("data:;charset=UTF-8,Hello, World!"_kj));
  KJ_ASSERT(dataUrl.getMimeType() == MimeType::PLAINTEXT);
  KJ_ASSERT(dataUrl.getData().asChars() == "Hello, World!"_kj);

  auto& val = KJ_REQUIRE_NONNULL(dataUrl.getMimeType().params().find("charset"_kj));
  KJ_ASSERT(val == "UTF-8"_kj);
}

struct Test {
  kj::StringPtr input;
  kj::StringPtr mimeType;
  kj::Array<const kj::byte> data;
};

KJ_TEST("DataUrl Web Platform Tests") {

  Test tests[] = {{
                    "data://test/,X"_kj,
                    "text/plain;charset=US-ASCII"_kj,
                    kj::heapArray<kj::byte>({88}),
                  },
    {
      "data://test:test/,X"_kj,
      nullptr,
      kj::heapArray<kj::byte>(0),
    },
    {
      "data:,X"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:"_kj,
      nullptr,
      kj::heapArray<kj::byte>(0),
    },
    {
      "data:text/html"_kj,
      nullptr,
      kj::heapArray<kj::byte>(0),
    },
    {
      "data:text/html    ;charset=x   "_kj,
      nullptr,
      kj::heapArray<kj::byte>(0),
    },
    {
      "data:,"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>(0),
    },
    {
      "data:,X#X"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:,%FF"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({255}),
    },
    {
      "data:text/plain,X"_kj,
      "text/plain"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:text/plain ,X"_kj,
      "text/plain"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:text/plain%20,X"_kj,
      "text/plain%20"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:text/plain\f,X"_kj,
      "text/plain%0c"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:text/plain%0C,X"_kj,
      "text/plain%0c"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:text/plain;,X"_kj,
      "text/plain"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:;x=x;charset=x,X"_kj,
      "text/plain;x=x;charset=x"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:;x=x,X"_kj,
      "text/plain;x=x"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:text/plain;charset=windows-1252,%C2%B1"_kj,
      "text/plain;charset=windows-1252"_kj,
      kj::heapArray<kj::byte>({194, 177}),
    },
    {
      "data:text/plain;Charset=UTF-8,%C2%B1"_kj,
      "text/plain;charset=UTF-8"_kj,
      kj::heapArray<kj::byte>({194, 177}),
    },
    {
      "data:text/plain;charset=windows-1252,áñçə💩"_kj,
      "text/plain;charset=windows-1252"_kj,
      kj::heapArray<kj::byte>({195, 161, 195, 177, 195, 167, 201, 153, 240, 159, 146, 169}),
    },
    {
      "data:text/plain;charset=UTF-8,áñçə💩"_kj,
      "text/plain;charset=UTF-8"_kj,
      kj::heapArray<kj::byte>({195, 161, 195, 177, 195, 167, 201, 153, 240, 159, 146, 169}),
    },
    {
      "data:image/gif,%C2%B1"_kj,
      "image/gif"_kj,
      kj::heapArray<kj::byte>({194, 177}),
    },
    {
      "data:IMAGE/gif,%C2%B1"_kj,
      "image/gif"_kj,
      kj::heapArray<kj::byte>({194, 177}),
    },
    {
      "data:IMAGE/gif;hi=x,%C2%B1"_kj,
      "image/gif;hi=x"_kj,
      kj::heapArray<kj::byte>({194, 177}),
    },
    {
      "data:IMAGE/gif;CHARSET=x,%C2%B1"_kj,
      "image/gif;charset=x"_kj,
      kj::heapArray<kj::byte>({194, 177}),
    },
    {
      "data: ,%FF"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({255}),
    },
    {
      "data:%20,%FF"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({255}),
    },
    {
      "data:\f,%FF"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({255}),
    },
    {
      "data:%1F,%FF"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({255}),
    },
    {
      "data:\u0000,%FF"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({255}),
    },
    {
      "data:%00,%FF"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({255}),
    },
    {
      "data:text/html  ,X"_kj,
      "text/html"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:text / html,X"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:†,X"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:†/†,X"_kj,
      "%e2%80%a0/%e2%80%a0"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:X,X"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:image/png,X X"_kj,
      "image/png"_kj,
      kj::heapArray<kj::byte>({88, 32, 88}),
    },
    {
      "data:application/javascript,X X"_kj,
      "application/javascript"_kj,
      kj::heapArray<kj::byte>({88, 32, 88}),
    },
    {
      "data:application/xml,X X"_kj,
      "application/xml"_kj,
      kj::heapArray<kj::byte>({88, 32, 88}),
    },
    {
      "data:text/javascript,X X"_kj,
      "text/javascript"_kj,
      kj::heapArray<kj::byte>({88, 32, 88}),
    },
    {
      "data:text/plain,X X"_kj,
      "text/plain"_kj,
      kj::heapArray<kj::byte>({88, 32, 88}),
    },
    {
      "data:unknown/unknown,X X"_kj,
      "unknown/unknown"_kj,
      kj::heapArray<kj::byte>({88, 32, 88}),
    },
    {
      "data:text/plain;a=\",\",X"_kj,
      "text/plain;a=\"\""_kj,
      kj::heapArray<kj::byte>({34, 44, 88}),
    },
    {
      "data:text/plain;a=%2C,X"_kj,
      "text/plain;a=%2C"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:;base64;base64,WA"_kj,
      "text/plain"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:x/x;base64;base64,WA"_kj,
      "x/x"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:x/x;base64;charset=x,WA"_kj,
      "x/x;charset=x"_kj,
      kj::heapArray<kj::byte>({87, 65}),
    },
    {
      "data:x/x;base64;charset=x;base64,WA"_kj,
      "x/x;charset=x"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:x/x;base64;base64x,WA"_kj,
      "x/x"_kj,
      kj::heapArray<kj::byte>({87, 65}),
    },
    {
      "data:;base64,W%20A"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:;base64,W%0CA"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:x;base64x,WA"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({87, 65}),
    },
    {
      "data:x;base64;x,WA"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({87, 65}),
    },
    {
      "data:x;base64=x,WA"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({87, 65}),
    },
    {
      "data:; base64,WA"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:;  base64,WA"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:  ;charset=x   ;  base64,WA"_kj,
      "text/plain;charset=x"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:;base64;,WA"_kj,
      "text/plain"_kj,
      kj::heapArray<kj::byte>({87, 65}),
    },
    {
      "data:;base64 ,WA"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:;base64   ,WA"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:;base 64,WA"_kj,
      "text/plain"_kj,
      kj::heapArray<kj::byte>({87, 65}),
    },
    {
      "data:;BASe64,WA"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:;%62ase64,WA"_kj,
      "text/plain"_kj,
      kj::heapArray<kj::byte>({87, 65}),
    },
    {
      "data:%3Bbase64,WA"_kj,
      "text/plain;charset=US-ASCII"_kj,
      kj::heapArray<kj::byte>({87, 65}),
    },
    {
      "data:;charset=x,X"_kj,
      "text/plain;charset=x"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:; charset=x,X"_kj,
      "text/plain;charset=x"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:;charset =x,X"_kj,
      "text/plain"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:;charset= x,X"_kj,
      "text/plain;charset=\" x\""_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:;charset=,X"_kj,
      "text/plain"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:;charset,X"_kj,
      "text/plain"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:;charset=\"x\",X"_kj,
      "text/plain;charset=x"_kj,
      kj::heapArray<kj::byte>({88}),
    },
    {
      "data:;CHARSET=\"X\",X"_kj,
      "text/plain;charset=X"_kj,
      kj::heapArray<kj::byte>({88}),
    }};

  auto testPtr = kj::arrayPtr<Test>(tests, 72);

  for (auto& test: testPtr) {
    if (test.mimeType == nullptr) {
      KJ_ASSERT(DataUrl::tryParse(test.input) == kj::none);
    } else {
      auto parsed = KJ_ASSERT_NONNULL(DataUrl::tryParse(test.input));
      KJ_ASSERT(parsed.getMimeType().toString() == test.mimeType);
      KJ_ASSERT(parsed.getData() == test.data);
    }
  }
}

struct Base64Test {
  kj::StringPtr input;
  kj::Array<kj::byte> expected;
};

KJ_TEST("DataUrl base64") {
  // Our base64 decoder is not very strict and way more forgiving than the
  // web platform's forgiving base64 decoder. That's just fine for us.
  // These cases were extracted from the Web Platform Tests for data urls
  // See: https://github.com/web-platform-tests/wpt/blob/master/fetch/data-urls/resources/
  Base64Test tests[] = {{""_kj, nullptr}, {"abcd"_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {" abcd"_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"abcd "_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {" abcd==="_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"abcd=== "_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"abcd ==="_kj, kj::heapArray<kj::byte>({105, 183, 29})}, {"a"_kj, nullptr},
    {"ab"_kj, kj::heapArray<kj::byte>({105})}, {"abc"_kj, kj::heapArray<kj::byte>({105, 183})},
    {"abcde"_kj, kj::heapArray<kj::byte>({105, 183, 29})}, {"𐀀"_kj, nullptr}, {"="_kj, nullptr},
    {"=="_kj, nullptr}, {"==="_kj, nullptr}, {"===="_kj, nullptr}, {"====="_kj, nullptr},
    {"a="_kj, nullptr}, {"a=="_kj, nullptr}, {"a==="_kj, nullptr}, {"a===="_kj, nullptr},
    {"a====="_kj, nullptr}, {"ab="_kj, kj::heapArray<kj::byte>({105})},
    {"ab=="_kj, kj::heapArray<kj::byte>({105})}, {"ab==="_kj, kj::heapArray<kj::byte>({105})},
    {"ab===="_kj, kj::heapArray<kj::byte>({105})}, {"ab====="_kj, kj::heapArray<kj::byte>({105})},
    {"abc="_kj, kj::heapArray<kj::byte>({105, 183})},
    {"abc=="_kj, kj::heapArray<kj::byte>({105, 183})},
    {"abc==="_kj, kj::heapArray<kj::byte>({105, 183})},
    {"abc===="_kj, kj::heapArray<kj::byte>({105, 183})},
    {"abc====="_kj, kj::heapArray<kj::byte>({105, 183})},
    {"abcd="_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"abcd=="_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"abcd==="_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"abcd===="_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"abcd====="_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"abcde="_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"abcde=="_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"abcde==="_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"abcde===="_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"abcde====="_kj, kj::heapArray<kj::byte>({105, 183, 29})}, {"=a"_kj, nullptr},
    {"=a="_kj, nullptr}, {"a=b"_kj, kj::heapArray<kj::byte>({105})},
    {"a=b="_kj, kj::heapArray<kj::byte>({105})}, {"ab=c"_kj, kj::heapArray<kj::byte>({105, 183})},
    {"ab=c="_kj, kj::heapArray<kj::byte>({105, 183})},
    {"abc=d"_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"abc=d="_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"ab\u000Bcd"_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"ab\u3000cd"_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"ab\u3001cd"_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"ab\tcd"_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"ab\ncd"_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"ab\fcd"_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"ab\rcd"_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"ab cd"_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"ab\u00a0cd"_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"ab\t\n\f\r cd"_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {" \t\n\f\r ab\t\n\f\r cd\t\n\f\r "_kj, kj::heapArray<kj::byte>({105, 183, 29})},
    {"ab\t\n\f\r =\t\n\f\r =\t\n\f\r "_kj, kj::heapArray<kj::byte>({105})}, {"A"_kj, nullptr},
    {"/A"_kj, kj::heapArray<kj::byte>({252})}, {"//A"_kj, kj::heapArray<kj::byte>({255, 240})},
    {"///A"_kj, kj::heapArray<kj::byte>({255, 255, 192})},
    {"////A"_kj, kj::heapArray<kj::byte>({255, 255, 255})}, {"/"_kj, nullptr},
    {"A/"_kj, kj::heapArray<kj::byte>({3})}, {"AA/"_kj, kj::heapArray<kj::byte>({0, 15})},
    {"AAAA/"_kj, kj::heapArray<kj::byte>({0, 0, 0})},
    {"AAA/"_kj, kj::heapArray<kj::byte>({0, 0, 63})},
    {"\u0000nonsense"_kj, kj::heapArray<kj::byte>({158, 137, 236, 122, 123, 30})},
    {"abcd\u0000nonsense"_kj, kj::heapArray<kj::byte>({105, 183, 29, 158, 137, 236, 122, 123, 30})},
    {"YQ"_kj, kj::heapArray<kj::byte>({97})}, {"YR"_kj, kj::heapArray<kj::byte>({97})},
    {"~~"_kj, nullptr}, {".."_kj, nullptr}, {"--"_kj, nullptr}, {"__"_kj, nullptr}};

  auto testPtr = kj::arrayPtr<Base64Test>(tests, 80);

  for (auto& test: testPtr) {
    auto input = kj::str("data:;base64,", test.input);

    auto url = KJ_ASSERT_NONNULL(DataUrl::tryParse(input));

    KJ_ASSERT(url.getData() == test.expected, test.input);
  }
}

KJ_TEST("Large Data URL") {
  auto str = kj::str(kj::repeat('a', 6000));
  auto url = kj::str("data:,", str);
  auto parsed = KJ_ASSERT_NONNULL(DataUrl::tryParse(url));
  KJ_ASSERT(parsed.getData().asChars() == str);
}

}  // namespace
}  // namespace workerd::api
