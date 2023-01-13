// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "util.h"
#include <kj/test.h>

namespace workerd::api {
namespace {

void expectRedacted(kj::StringPtr input, kj::StringPtr expected) {
  KJ_EXPECT(redactUrl(input) == expected, redactUrl(input), expected);
}
void expectUnredacted(kj::StringPtr input) {
  KJ_EXPECT(redactUrl(input) == input, redactUrl(input), input);
}

void expectContentTypeParameter(kj::StringPtr input, kj::StringPtr param, kj::StringPtr expected) {
  auto res = readContentTypeParameter(input, param);
  auto value = KJ_ASSERT_NONNULL(res);
  KJ_EXPECT(value == expected);
}

KJ_TEST("redactUrl can detect hex ids") {
  // no id:
  expectUnredacted(""_kj);
  expectUnredacted("https://domain/path?a=1&b=2"_kj);

  expectRedacted(
      "https://domain/0123456789abcdef0123456789abcdef/x"_kj,
      "https://domain/REDACTED/x"_kj);
  expectRedacted(
      "https://domain/0123456789abcdef-0123456789abcdef/x"_kj,
      "https://domain/REDACTED/x"_kj);

  // not long enough:
  expectUnredacted("https://domain/0123456789abcdef0123456789abcde/x"_kj);
  expectUnredacted("https://domain/0123456789-abcdef-0123456789-abcde/x"_kj);
  expectUnredacted("https://domain/0123456789ABCDEF0123456789ABCDE/x"_kj);
  expectUnredacted("https://domain/0123456789_ABCDEF_0123456789_ABCDE/x"_kj);

  // contains non-hex character:
  expectUnredacted("https://domain/0123456789abcdef0123456789abcdefg/x"_kj);
}

KJ_TEST("redactUrl can detect base64 ids") {
  expectRedacted("https://domain/01234567890123456azAZ/x"_kj, "https://domain/REDACTED/x"_kj);

  // not long enough:
  expectUnredacted("https://domain/0123456789012345azAZ/x"_kj);

  // not enough lowercase:
  expectUnredacted("https://domain/012345678901234567zAZ/x"_kj);

  // not enough uppercase:
  expectUnredacted("https://domain/012345678901234567azZ/x"_kj);

  // not enough digits:
  expectUnredacted("https://domain/IThinkIShallNeverSee0/x"_kj);
}

KJ_TEST("readContentTypeParameter can fetch boundary parameter") {

  // normal
  expectContentTypeParameter(
    "multipart/form-data; boundary=\"__boundary__\""_kj,
    "boundary"_kj,
    "__boundary__"_kj
  );

  // multiple params
  expectContentTypeParameter(
    "multipart/form-data; charset=utf-8; boundary=\"__boundary__\""_kj,
    "boundary"_kj,
    "__boundary__"_kj
  );

 // param name inside value of other param
  expectContentTypeParameter(
    "multipart/form-data; charset=\"boundary=;\"; boundary=\"__boundary__\""_kj,
    "boundary"_kj,
    "__boundary__"_kj
  );

  // ensure param is not found
  KJ_ASSERT(readContentTypeParameter(
    "multipart/form-data; charset=\"boundary=;\"; boundary=\"__boundary__\""_kj,
    "boundary1"_kj
  ) == nullptr);

  // no quotes
  expectContentTypeParameter(
    "multipart/form-data; charset=\"boundary=;\"; boundary=__boundary__"_kj,
    "boundary"_kj,
    "__boundary__"_kj
  );

  // attribute names are case-insensitive, but values are not
  expectContentTypeParameter(
    "multipart/form-data; charset=\"boundary=;\"; boundary=__Boundary__"_kj,
    "Boundary"_kj,
    "__Boundary__"_kj
  );

  // different order
  expectContentTypeParameter(
    "multipart/form-data; boundary=\"__boundary__\"; charset=utf-8"_kj,
    "boundary"_kj,
    "__boundary__"_kj
  );

  // bogus parameter
  expectContentTypeParameter(
    "multipart/form-data; foo=123; boundary=\"__boundary__\""_kj,
    "boundary"_kj,
    "__boundary__"_kj
  );

  // quoted-string
  expectContentTypeParameter(
    R"(multipart/form-data; foo="\"boundary=bar\""; boundary="realboundary")",
    "boundary"_kj,
    "realboundary"_kj
  );

  // handle non-closing quotes
  KJ_ASSERT(readContentTypeParameter(
    R"(multipart/form-data; charset="boundary=;\"; boundary="__boundary__")",
    "boundary"_kj
  ) == nullptr);

  // handle non-closing quotes on wanted param
  KJ_ASSERT(readContentTypeParameter(
    R"(multipart/form-data; charset="boundary=;"; boundary="__boundary__\")",
    "boundary"_kj
  ) == nullptr);

  // handle incorrect quotes
  KJ_ASSERT(readContentTypeParameter(
    R"(multipart/form-data; charset=\"boundary=;\"; boundary=\"__boundary__\")",
    "boundary"_kj
  ) == nullptr);

  // spurious whitespace before ;
  expectContentTypeParameter(
    "multipart/form-data; boundary=asdf ;foo=bar"_kj,
    "boundary"_kj,
    "asdf"_kj
  );

  // spurious whitespace before ; with quotes
  expectContentTypeParameter(
    "multipart/form-data; boundary=\"asdf\" ;foo=bar"_kj,
    "boundary"_kj,
    "asdf"_kj
  );

  // all whitespace
  KJ_ASSERT(readContentTypeParameter(
    "multipart/form-data; boundary= ;foo=bar"_kj,
    "boundary"_kj
  ) == nullptr);

  // all whitespace with quotes
  KJ_ASSERT(readContentTypeParameter(
    "multipart/form-data; boundary="" ;foo=bar"_kj,
    "boundary"_kj
  ) == nullptr);

  // terminal escape character after quote
  KJ_ASSERT(readContentTypeParameter(
    R"(multipart/form-data; foo="\)",
    "boundary"_kj
  ) == nullptr);

  // space before value
  expectContentTypeParameter(
    "multipart/form-data; boundary= a"_kj,
    "boundary"_kj,
    " a"_kj
  );

  // space before value with quotes
  expectContentTypeParameter(
    "multipart/form-data; boundary=\" a\""_kj,
    "boundary"_kj,
    " a"_kj
  );

  // space before ; on another param
  expectContentTypeParameter(
    "multipart/form-data; foo=\"bar\" ;boundary=asdf"_kj,
    "boundary"_kj,
    "asdf"_kj
  );

  // space before ; on another param with quotes
  expectContentTypeParameter(
    "multipart/form-data; foo=\"bar\" ;boundary=\"asdf\""_kj,
    "boundary"_kj,
    "asdf"_kj
  );

  // space before ; on another param no quotes
  expectContentTypeParameter(
    "multipart/form-data; foo=bar ;boundary=asdf"_kj,
    "boundary"_kj,
    "asdf"_kj
  );

  // space before ; on another param quotes on wanted param
  expectContentTypeParameter(
    "multipart/form-data; foo=bar ;boundary=\"asdf\""_kj,
    "boundary"_kj,
    "asdf"_kj
  );

}

}  // namespace
}  // namespace workerd::api
