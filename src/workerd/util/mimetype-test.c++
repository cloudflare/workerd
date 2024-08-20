// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "mimetype.h"
#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("Basic MimeType parsing works") {
  struct TestCase {
    kj::StringPtr input;
    kj::StringPtr type;
    kj::StringPtr subtype;
    kj::StringPtr output;
    kj::Maybe<kj::Array<MimeType::MimeParams::Entry>> params;
  };
  static const TestCase kTests[] = {
    {
      .input = "text/plain"_kj,
      .type = "text"_kj,
      .subtype = "plain"_kj,
      .output = "text/plain"_kj,
    },
    {
      .input = "\r\t\n TeXt/PlAiN \t\r\n"_kj,
      .type = "text"_kj,
      .subtype = "plain"_kj,
      .output = "text/plain"_kj,
    },
    {.input = "text/plain; charset=utf-8"_kj,
      .type = "text"_kj,
      .subtype = "plain"_kj,
      .output = "text/plain;charset=utf-8"_kj,
      .params = kj::arr(MimeType::MimeParams::Entry{kj::str("charset"), kj::str("utf-8")})},
    {.input = "text/plain; charset=\"utf-8\""_kj,
      .type = "text"_kj,
      .subtype = "plain"_kj,
      .output = "text/plain;charset=utf-8"_kj,
      .params = kj::arr(MimeType::MimeParams::Entry{kj::str("charset"), kj::str("utf-8")})},
    {.input = "text/plain; charset=\"utf-8\"; \r\n\t"_kj,
      .type = "text"_kj,
      .subtype = "plain"_kj,
      .output = "text/plain;charset=utf-8"_kj,
      .params = kj::arr(MimeType::MimeParams::Entry{kj::str("charset"), kj::str("utf-8")})},
    {.input = "text/plain; charset=\"utf-8\"; \r\n\ta=b"_kj,
      .type = "text"_kj,
      .subtype = "plain"_kj,
      .output = "text/plain;charset=utf-8;a=b"_kj,
      .params = kj::arr(MimeType::MimeParams::Entry{kj::str("charset"), kj::str("utf-8")},
          MimeType::MimeParams::Entry{kj::str("a"), kj::str("b")})},
    {.input = "text/plain; charset=utf-8; a=b;a=a"_kj,
      .type = "text"_kj,
      .subtype = "plain"_kj,
      .output = "text/plain;charset=utf-8;a=b"_kj,
      .params = kj::arr(MimeType::MimeParams::Entry{kj::str("charset"), kj::str("utf-8")},
          MimeType::MimeParams::Entry{kj::str("a"), kj::str("b")})},
  };

  for (auto& test: kTests) {
    auto mimeType = KJ_ASSERT_NONNULL(MimeType::tryParse(test.input));
    KJ_ASSERT(mimeType.type() == test.type);
    KJ_ASSERT(mimeType.subtype() == test.subtype);
    KJ_ASSERT(mimeType.toString() == test.output);

    KJ_IF_SOME(params, test.params) {
      for (auto& param: params) {
        auto& value = KJ_ASSERT_NONNULL(mimeType.params().find(param.key));
        KJ_ASSERT(value == param.value);
      }
    }
  }

  struct ErrorTestCase {
    kj::StringPtr input;
  };
  static const ErrorTestCase kErrorTests[] = {
    {""},
    {"text"},
    {"text/"},
    {"/plain"},
    {"/"},
    {" a/\x12"},
    {" \x12/a"},
    {" text/ plain"},
    {" text /plain"},
    {" text / plain"},
    {";charset=utf-8"},
    {"javascript"},
  };

  for (auto& test: kErrorTests) {
    KJ_ASSERT(MimeType::tryParse(test.input) == kj::none, test.input);
  }
}

KJ_TEST("Building MimeType works") {
  MimeType type("text", "plain");

  KJ_ASSERT(!type.addParam(""_kj, ""_kj));
  KJ_ASSERT(!type.addParam("\x12"_kj, ""_kj));
  KJ_ASSERT(!type.addParam("B"_kj, "\12"_kj));

  KJ_ASSERT(type.addParam("A"_kj, "b"_kj));
  KJ_ASSERT(type.addParam("Z"_kj, "b"_kj));
  type.eraseParam("Z");

  KJ_ASSERT(type.toString() == "text/plain;a=b");

  KJ_ASSERT(type.params().find("a"_kj) != kj::none);
  KJ_ASSERT(type.params().find("b"_kj) == kj::none);
  KJ_ASSERT(type.params().find("z"_kj) == kj::none);

  // Comparing based solely on type/subtype works
  KJ_ASSERT(MimeType::PLAINTEXT == type);
}

KJ_TEST("WHATWG tests") {
  struct Test {
    kj::StringPtr input;
    kj::Maybe<kj::StringPtr> output;
  };

  static const Test kTests[] = {{
                                  .input = "text/html;charset=gbk"_kj,
                                  .output = "text/html;charset=gbk"_kj,
                                },
    {
      .input = "TEXT/HTML;CHARSET=GBK"_kj,
      .output = "text/html;charset=GBK"_kj,
    },
    // Legacy comment syntax
    {
      .input = "text/html;charset=gbk("_kj,
      .output = "text/html;charset=\"gbk(\""_kj,
    },
    {
      .input = "text/html;x=(;charset=gbk"_kj,
      .output = "text/html;x=\"(\";charset=gbk"_kj,
    },
    // "Duplicate parameter",
    {
      .input = "text/html;charset=gbk;charset=windows-1255"_kj,
      .output = "text/html;charset=gbk"_kj,
    },
    {
      .input = "text/html;charset=();charset=GBK"_kj,
      .output = "text/html;charset=\"()\""_kj,
    },
    // "Spaces",
    {
      .input = "text/html;charset =gbk"_kj,
      .output = "text/html"_kj,
    },
    {
      .input = "text/html ;charset=gbk"_kj,
      .output = "text/html;charset=gbk"_kj,
    },
    {
      .input = "text/html; charset=gbk"_kj,
      .output = "text/html;charset=gbk"_kj,
    },
    {
      .input = "text/html;charset= gbk"_kj,
      .output = "text/html;charset=\" gbk\""_kj,
    },
    {
      .input = "text/html;charset= \"gbk\""_kj,
      .output = "text/html;charset=\" \\\"gbk\\\"\""_kj,
    },
    // "0x0B and 0x0C",
    {
      .input = "text/html;charset=\u000Bgbk"_kj,
      .output = "text/html"_kj,
    },
    {
      .input = "text/html;charset=\u000Cgbk"_kj,
      .output = "text/html"_kj,
    },
    {
      .input = "text/html;\u000Bcharset=gbk"_kj,
      .output = "text/html"_kj,
    },
    {
      .input = "text/html;\u000Ccharset=gbk"_kj,
      .output = "text/html"_kj,
    },
    // "Single quotes are a token, not a delimiter",
    {
      .input = "text/html;charset='gbk'"_kj,
      .output = "text/html;charset='gbk'"_kj,
    },
    {
      .input = "text/html;charset='gbk"_kj,
      .output = "text/html;charset='gbk"_kj,
    },
    {
      .input = "text/html;charset=gbk'"_kj,
      .output = "text/html;charset=gbk'"_kj,
    },
    {
      .input = "text/html;charset=';charset=GBK"_kj,
      .output = "text/html;charset='"_kj,
    },
    // "Invalid parameters",
    {
      .input = "text/html;test;charset=gbk"_kj,
      .output = "text/html;charset=gbk"_kj,
    },
    {
      .input = "text/html;test=;charset=gbk"_kj,
      .output = "text/html;charset=gbk"_kj,
    },
    {
      .input = "text/html;';charset=gbk"_kj,
      .output = "text/html;charset=gbk"_kj,
    },
    {
      .input = "text/html;\";charset=gbk"_kj,
      .output = "text/html;charset=gbk"_kj,
    },
    {
      .input = "text/html ; ; charset=gbk"_kj,
      .output = "text/html;charset=gbk"_kj,
    },
    {
      .input = "text/html;;;;charset=gbk"_kj,
      .output = "text/html;charset=gbk"_kj,
    },
    {
      .input = "text/html;charset= \"\u007F;charset=GBK"_kj,
      .output = "text/html;charset=GBK"_kj,
    },
    {
      .input = "text/html;charset=\"\u007F;charset=foo\";charset=GBK"_kj,
      .output = "text/html;charset=GBK"_kj,
    },
    // "Double quotes",
    {
      .input = "text/html;charset=\"gbk\""_kj,
      .output = "text/html;charset=gbk"_kj,
    },
    {
      .input = "text/html;charset=\"gbk"_kj,
      .output = "text/html;charset=gbk"_kj,
    },
    {
      .input = "text/html;charset=gbk\""_kj,
      .output = "text/html;charset=\"gbk\\\"\""_kj,
    },
    {
      .input = "text/html;charset=\" gbk\""_kj,
      .output = "text/html;charset=\" gbk\""_kj,
    },
    {
      .input = "text/html;charset=\"gbk \""_kj,
      .output = "text/html;charset=\"gbk \""_kj,
    },
    {
      .input = "text/html;charset=\"\\ gbk\""_kj,
      .output = "text/html;charset=\" gbk\""_kj,
    },
    {
      .input = "text/html;charset=\"\\g\\b\\k\""_kj,
      .output = "text/html;charset=gbk"_kj,
    },
    {
      .input = "text/html;charset=\"gbk\"x"_kj,
      .output = "text/html;charset=gbk"_kj,
    },
    {
      .input = "text/html;charset=\"\";charset=GBK"_kj,
      .output = "text/html;charset=\"\""_kj,
    },
    {
      .input = "text/html;charset=\";charset=GBK"_kj,
      .output = "text/html;charset=\";charset=GBK\""_kj,
    },
    // "Unexpected code points",
    {
      .input = "text/html;charset={gbk}"_kj,
      .output = "text/html;charset=\"{gbk}\""_kj,
    },
    // "Parameter name longer than 127",
    {
      .input =
          "text/html;0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789=x;charset=gbk"_kj,
      .output =
          "text/html;0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789=x;charset=gbk"_kj,
    },
    // "type/subtype longer than 127",
    {
      .input =
          "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789/0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"_kj,
      .output =
          "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789/0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"_kj,
    },
    // "Valid",
    {
      .input =
          "!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz/!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz;!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz=!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"_kj,
      .output =
          "!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz/!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz;!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz=!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"_kj,
    },
    // TODO(soon): Extreme edge case, we currently don't pass it but not too concerned.
    // {
    //   .input = "x/x;x=\"\t !\\\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\\\]^_`abcdefghijklmnopqrstuvwxyz{|}~\u0080\u0081\u0082\u0083\u0084\u0085\u0086\u0087\u0088\u0089\u008A\u008B\u008C\u008D\u008E\u008F\u0090\u0091\u0092\u0093\u0094\u0095\u0096\u0097\u0098\u0099\u009A\u009B\u009C\u009D\u009E\u009F\u00A0\u00A1\u00A2\u00A3\u00A4\u00A5\u00A6\u00A7\u00A8\u00A9\u00AA\u00AB\u00AC\u00AD\u00AE\u00AF\u00B0\u00B1\u00B2\u00B3\u00B4\u00B5\u00B6\u00B7\u00B8\u00B9\u00BA\u00BB\u00BC\u00BD\u00BE\u00BF\u00C0\u00C1\u00C2\u00C3\u00C4\u00C5\u00C6\u00C7\u00C8\u00C9\u00CA\u00CB\u00CC\u00CD\u00CE\u00CF\u00D0\u00D1\u00D2\u00D3\u00D4\u00D5\u00D6\u00D7\u00D8\u00D9\u00DA\u00DB\u00DC\u00DD\u00DE\u00DF\u00E0\u00E1\u00E2\u00E3\u00E4\u00E5\u00E6\u00E7\u00E8\u00E9\u00EA\u00EB\u00EC\u00ED\u00EE\u00EF\u00F0\u00F1\u00F2\u00F3\u00F4\u00F5\u00F6\u00F7\u00F8\u00F9\u00FA\u00FB\u00FC\u00FD\u00FE\u00FF\""_kj,
    //   .output = "x/x;x=\"\t !\\\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\\\]^_`abcdefghijklmnopqrstuvwxyz{|}~\u0080\u0081\u0082\u0083\u0084\u0085\u0086\u0087\u0088\u0089\u008A\u008B\u008C\u008D\u008E\u008F\u0090\u0091\u0092\u0093\u0094\u0095\u0096\u0097\u0098\u0099\u009A\u009B\u009C\u009D\u009E\u009F\u00A0\u00A1\u00A2\u00A3\u00A4\u00A5\u00A6\u00A7\u00A8\u00A9\u00AA\u00AB\u00AC\u00AD\u00AE\u00AF\u00B0\u00B1\u00B2\u00B3\u00B4\u00B5\u00B6\u00B7\u00B8\u00B9\u00BA\u00BB\u00BC\u00BD\u00BE\u00BF\u00C0\u00C1\u00C2\u00C3\u00C4\u00C5\u00C6\u00C7\u00C8\u00C9\u00CA\u00CB\u00CC\u00CD\u00CE\u00CF\u00D0\u00D1\u00D2\u00D3\u00D4\u00D5\u00D6\u00D7\u00D8\u00D9\u00DA\u00DB\u00DC\u00DD\u00DE\u00DF\u00E0\u00E1\u00E2\u00E3\u00E4\u00E5\u00E6\u00E7\u00E8\u00E9\u00EA\u00EB\u00EC\u00ED\u00EE\u00EF\u00F0\u00F1\u00F2\u00F3\u00F4\u00F5\u00F6\u00F7\u00F8\u00F9\u00FA\u00FB\u00FC\u00FD\u00FE\u00FF\""_kj,
    // },
    // "End-of-file handling",
    {
      .input = "x/x;test"_kj,
      .output = "x/x"_kj,
    },
    // TODO(soon): Another edge case, we currently don't pass it but not too concerned.
    // {
    //  .input = "x/x;test=\"\\"_kj,
    //  .output = "x/x;test=\"\\\\\""_kj,
    // },
    // "Whitespace (not handled by generated-mime-types.json or above)",
    {
      .input = "x/x;x= "_kj,
      .output = "x/x"_kj,
    },
    {
      .input = "x/x;x=\t"_kj,
      .output = "x/x"_kj,
    },
    {
      .input = "x/x\n\r\t ;x=x"_kj,
      .output = "x/x;x=x"_kj,
    },
    {
      .input = "\n\r\t x/x;x=x\n\r\t "_kj,
      .output = "x/x;x=x"_kj,
    },
    {
      .input = "x/x;\n\r\t x=x\n\r\t ;x=y"_kj,
      .output = "x/x;x=x"_kj,
    },
    // "Latin1",
    {
      .input = "text/html;test=\u00FF;charset=gbk"_kj,
      .output = "text/html;test=\"\u00FF\";charset=gbk"_kj,
    },
    // ">Latin1",
    // TODO(soon): Another edge case, we currently don't pass it but not too concerned.
    // {
    //   .input = "x/x;test=\uFFFD;x=x"_kj,
    //   .output = "x/x;x=x"_kj,
    // },
    // "Failure",
    {
      .input = "\u000Bx/x"_kj,
    },
    {.input = "\u000Cx/x"_kj}, {.input = "x/x\u000B"_kj}, {.input = "x/x\u000C"_kj},
    {.input = ""_kj}, {.input = "\t"_kj}, {.input = "/"_kj}, {.input = "bogus"_kj},
    {.input = "bogus/"_kj}, {.input = "bogus/ "_kj}, {.input = "bogus/bogus/;"_kj},
    {.input = "</>"_kj}, {.input = "(/)"_kj}, {.input = "ÿ/ÿ"_kj},
    {.input = "text/html(;doesnot=matter"_kj}, {.input = "{/}"_kj}, {.input = "\u0100/\u0100"_kj},
    {.input = "text /html"_kj}, {.input = "text/ html"_kj}, {.input = "\"text/html\""_kj}};

  for (const auto& test: kTests) {
    KJ_IF_SOME(output, test.output) {
      auto result = KJ_ASSERT_NONNULL(MimeType::tryParse(test.input));
      KJ_ASSERT(result.toString() == output);
    } else {
      KJ_ASSERT(MimeType::tryParse(test.input) == kj::none);
    }
  }

  KJ_ASSERT(MimeType::JSON ==
      KJ_ASSERT_NONNULL(MimeType::tryParse("application/json;charset=nothing"_kj)));
  KJ_ASSERT(MimeType::JSON == KJ_ASSERT_NONNULL(MimeType::tryParse("application/json;"_kj)));
  KJ_ASSERT(MimeType::JSON ==
      KJ_ASSERT_NONNULL(MimeType::tryParse("application/json;char=\"UTF-8\""_kj)));
  KJ_ASSERT(MimeType::isJson(MimeType::JSON));
  KJ_ASSERT(MimeType::isJson(MimeType::MANIFEST_JSON));
  KJ_ASSERT(MimeType::isJavascript(MimeType::JAVASCRIPT));
  KJ_ASSERT(MimeType::isJavascript(MimeType::XJAVASCRIPT));
  KJ_ASSERT(MimeType::isJavascript(MimeType::TEXT_JAVASCRIPT));

  KJ_ASSERT(MimeType::isText(MimeType::PLAINTEXT));
  KJ_ASSERT(MimeType::isText(MimeType::JSON));
  KJ_ASSERT(MimeType::isText(MimeType::JAVASCRIPT));
  KJ_ASSERT(MimeType::isText(MimeType::XJAVASCRIPT));
  KJ_ASSERT(MimeType::isText(
      KJ_ASSERT_NONNULL(MimeType::tryParse("application/json; charset=\"utf-8\""_kj))));
}

KJ_TEST("Extract Mime Type") {
  // These are taken from the fetch spec
  // https://fetch.spec.whatwg.org/#concept-header-extract-mime-type
  {
    auto mimeType = KJ_ASSERT_NONNULL(MimeType::extract("text/plain;charset=gbk, text/html"_kj));
    KJ_ASSERT(mimeType == MimeType::HTML);
  }

  {
    auto mimeType =
        KJ_ASSERT_NONNULL(MimeType::extract("text/html;charset=gbk;a=b, text/html;x=y"));
    KJ_ASSERT(mimeType.toString() == "text/html;x=y;charset=gbk");
  }

  {
    auto mimeType =
        KJ_ASSERT_NONNULL(MimeType::extract("text/html;charset=gbk, x/x, text/html;x=y"));
    KJ_ASSERT(mimeType.toString() == "text/html;x=y");
  }

  {
    auto mimeType = KJ_ASSERT_NONNULL(MimeType::extract("text/html, cannot parse"));
    KJ_ASSERT(mimeType.toString() == "text/html");
  }

  {
    auto mimeType = KJ_ASSERT_NONNULL(MimeType::extract("text/html, */*"));
    KJ_ASSERT(mimeType.toString() == "text/html");
  }

  {
    auto mimeType = KJ_ASSERT_NONNULL(MimeType::extract("text/html, "));
    KJ_ASSERT(mimeType.toString() == "text/html");
  }

  {
    // An odd edge case where the parameter value contains an escaped quote and escaped
    // comma in the value.
    auto mimeType = KJ_ASSERT_NONNULL(MimeType::extract("text/html;a=\\\"not-quoted\\,, foo/bar"));
    KJ_ASSERT(mimeType.toString() == "foo/bar");
  }

  // These are taken from the web platform tests
  struct Test {
    kj::StringPtr input;
    kj::StringPtr encoding;
    kj::StringPtr result;
  };

  Test tests[] = {
    Test{
      .input = ", text/plain"_kj,
      .encoding = nullptr,
      .result = "text/plain"_kj,
    },
    Test{
      .input = "text/plain, "_kj,
      .encoding = nullptr,
      .result = "text/plain"_kj,
    },
    {
      .input = "text/html, text/plain"_kj,
      .encoding = nullptr,
      .result = "text/plain"_kj,
    },
    {
      .input = "text/plain;charset=gbk, text/html"_kj,
      .encoding = nullptr,
      .result = "text/html"_kj,
    },
    {
      .input = "text/plain;charset=gbk, text/html;charset=windows-1254"_kj,
      .encoding = "windows-1254"_kj,
      .result = "text/html;charset=windows-1254"_kj,
    },
    {
      .input = "text/plain;charset=gbk, text/plain"_kj,
      .encoding = "gbk"_kj,
      .result = "text/plain;charset=gbk"_kj,
    },
    {
      .input = "text/plain;charset=gbk, text/plain;charset=windows-1252"_kj,
      .encoding = "windows-1252"_kj,
      .result = "text/plain;charset=windows-1252"_kj,
    },
    {
      .input = "text/plain;charset=gbk;x=foo, text/plain"_kj,
      .encoding = "gbk"_kj,
      .result = "text/plain;charset=gbk"_kj,
    },
    {
      .input = "text/html;charset=gbk, text/plain, text/html"_kj,
      .encoding = nullptr,
      .result = "text/html"_kj,
    },
    {
      .input = "text/plain, */*"_kj,
      .encoding = nullptr,
      .result = "text/plain"_kj,
    },
    {
      .input = "text/html, */*"_kj,
      .encoding = nullptr,
      .result = "text/html"_kj,
    },
    {
      .input = "*/*, text/html"_kj,
      .encoding = nullptr,
      .result = "text/html"_kj,
    },
    {
      .input = "text/plain, */*;charset=gbk"_kj,
      .encoding = nullptr,
      .result = "text/plain"_kj,
    },
    {
      .input = "text/html, */*;charset=gbk"_kj,
      .encoding = nullptr,
      .result = "text/html"_kj,
    },
    {
      .input = "text/html;\", \", text/plain"_kj,
      .encoding = nullptr,
      .result = "text/plain"_kj,
    },
    {
      .input = "text/html;charset=gbk, text/html;x=\",text/plain"_kj,
      .encoding = "gbk"_kj,
      .result = "text/html;x=\",text/plain\";charset=gbk"_kj,
    },
    {
      .input = "text/html;x=\", text/plain"_kj,
      .encoding = nullptr,
      .result = "text/html;x=\", text/plain\""_kj,
    },
    {
      .input = "text/html;\", text/plain"_kj,
      .encoding = nullptr,
      .result = "text/html"_kj,
    },
    {
      .input = "text/html;\", \\\", text/plain"_kj,
      .encoding = nullptr,
      .result = "text/html"_kj,
    },
    {
      // This is actually three separate Content-Type header fields concated together
      // into a list. The original values are:
      //  Content-Type: text/html;\"
      //  Content-Type: \\\"
      //  Content-Type: text/plain, \";charset=GBK
      //
      // When combined using the typical rules for combining multiple headers, the result
      // actually ends up being just a single mime type with an invalid parameter.
      .input = "text/html;\", \\\", text/plain, \";charset=GBK"_kj,
      .encoding = "GBK"_kj,
      .result = "text/html;charset=GBK"_kj,
    },
  };
  auto ptr = kj::ArrayPtr<Test>(tests, sizeof(tests) / sizeof(Test));

  for (auto& test: ptr) {
    auto parsed = KJ_ASSERT_NONNULL(MimeType::extract(test.input));
    KJ_ASSERT(parsed.toString() == test.result);
    if (test.encoding != nullptr) {
      KJ_ASSERT(KJ_ASSERT_NONNULL(parsed.params().find("charset"_kj)) == test.encoding);
    }
  }
}

}  // namespace
}  // namespace workerd
