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

}  // namespace
}  // namespace workerd::api
