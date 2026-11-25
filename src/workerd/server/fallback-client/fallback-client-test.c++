// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Checks the fallback-client crate's requests and its reading of responses against capnp's
// JsonCodec and kj::Url, used here as the C++ client used them before the crate replaced it: the
// same inputs must give the same bytes on the wire and the same modules.

#include <workerd/server/fallback-client/test_bridge.rs.h>
#include <workerd/server/workerd.capnp.h>

#include <kj-rs/kj-rs.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/compat/url.h>
#include <kj/test.h>

namespace workerd::fallback::test {
namespace {

using kj_rs::Rust;
using Module = server::config::Worker::Module;

kj::StringPtr ptr(kj::ArrayPtr<const char> chars) {
  // The test's inputs are string literals, NUL-terminated.
  return kj::StringPtr(chars.begin(), chars.size());
}

// The C++ client's V1 request target and the name it expected (`actualSpecifier`).
struct V1Request {
  kj::String target;
  kj::String expectedName;
};

V1Request cppV1(kj::StringPtr specifier, kj::StringPtr rawSpecifier, kj::StringPtr referrer) {
  bool prefixed = false;
  kj::Url url;
  kj::StringPtr actualSpecifier = nullptr;
  KJ_IF_SOME(pos, specifier.findLast('/')) {
    auto segment = specifier.slice(pos + 1);
    if (segment.startsWith("node:") || segment.startsWith("cloudflare:") ||
        segment.startsWith("workerd:")) {
      actualSpecifier = segment;
      url.query.add(kj::Url::QueryParam{.name = kj::str("specifier"), .value = kj::str(segment)});
      prefixed = true;
    }
  }
  if (!prefixed) {
    actualSpecifier = specifier;
    if (actualSpecifier.startsWith("/")) {
      actualSpecifier = specifier.slice(1);
    }
    url.query.add(kj::Url::QueryParam{kj::str("specifier"), kj::str(specifier)});
  }
  url.query.add(kj::Url::QueryParam{kj::str("referrer"), kj::str(referrer)});
  url.query.add(kj::Url::QueryParam{kj::str("rawSpecifier"), kj::str(rawSpecifier)});
  return {url.toString(kj::Url::HTTP_REQUEST), kj::str(actualSpecifier)};
}

// The C++ client's V2 request body.
kj::String cppV2(kj::StringPtr specifier,
    kj::StringPtr rawSpecifier,
    kj::StringPtr referrer,
    kj::StringPtr attributeType) {
  capnp::JsonCodec json;
  capnp::MallocMessageBuilder message;
  auto request = message.initRoot<server::config::FallbackServiceRequest>();
  request.setType("import");
  request.setSpecifier(specifier);
  request.setReferrer(referrer);
  if (rawSpecifier != nullptr) {
    request.setRawSpecifier(rawSpecifier);
  }
  if (attributeType != nullptr) {
    auto attrs = request.initAttributes(1);
    attrs[0].setName("type");
    attrs[0].setValue(attributeType);
  }
  return json.encode(request);
}

// The C++ client's `handleReturnPayload()` for a 200 response: the module, canonicalized, or
// kj::none if it failed.
kj::Maybe<kj::Array<capnp::word>> cppDecode(
    kj::ArrayPtr<const char> payload, kj::StringPtr specifier) {
  kj::Maybe<kj::Array<capnp::word>> result;
  auto exception = kj::runCatchingExceptions([&]() {
    capnp::MallocMessageBuilder message;
    capnp::JsonCodec json;
    json.handleByAnnotation<Module>();
    auto module = message.initRoot<Module>();
    json.decode(payload, module);
    if (module.hasName()) {
      if (module.getName() != specifier) return;
    } else {
      module.setName(specifier);
    }
    result = capnp::canonicalize(module.asReader());
  });
  if (exception != kj::none) return kj::none;
  return kj::mv(result);
}

kj::Maybe<kj::Array<capnp::word>> rustDecode(
    kj::ArrayPtr<const char> payload, kj::StringPtr specifier) {
  auto decoded = decode_module(payload.asBytes().as<Rust>(), specifier.asBytes().as<Rust>());
  if (!decoded.ok) return kj::none;
  auto bytes = kj::from<Rust>(decoded.module);
  auto words = kj::heapArray<capnp::word>(bytes.size() / sizeof(capnp::word));
  words.asBytes().copyFrom(bytes.first(words.asBytes().size()));
  capnp::FlatArrayMessageReader reader(words, {.traversalLimitInWords = kj::maxValue});
  return capnp::canonicalize(reader.getRoot<Module>());
}

KJ_TEST("V1 request targets match kj::Url") {
  struct Case {
    kj::StringPtr specifier;
    kj::StringPtr rawSpecifier;
    kj::StringPtr referrer;
  };
  Case cases[] = {
    {"/module.js"_kj, "./module.js"_kj, "/worker.js"_kj},
    {"/module.js"_kj, ""_kj, ""_kj},
    {"module.js"_kj, "a b+c&d=e?f#g%h"_kj, "/dir/worker.js"_kj},
    {"/bundle/node:fs"_kj, "node:fs"_kj, "/worker.js"_kj},
    {"/cloudflare:sockets"_kj, "cloudflare:sockets"_kj, "/w.js"_kj},
    {"/a/workerd:x"_kj, ""_kj, "/w.js"_kj},
    {"node:fs"_kj, ""_kj, ""_kj},
    {"/polyfills/node:grow/x"_kj, ""_kj, ""_kj},
    {"/caf\xc3\xa9/\xff\x01~!*'()"_kj, "\t\n"_kj, "/\x7f"_kj},
  };
  for (auto& c: cases) {
    auto cpp = cppV1(c.specifier, c.rawSpecifier, c.referrer);
    auto rust =
        encode_request(false, c.specifier.asBytes().as<Rust>(), c.rawSpecifier.asBytes().as<Rust>(),
            c.referrer.asBytes().as<Rust>(), ""_kj.asBytes().as<Rust>());
    KJ_EXPECT(kj::str(kj::from<Rust>(rust.target)) == cpp.target, c.specifier);
    KJ_EXPECT(kj::from<Rust>(rust.expected_name) == cpp.expectedName.asBytes(), c.specifier);
  }
}

KJ_TEST("V2 request bodies match JsonCodec") {
  struct Case {
    kj::StringPtr specifier;
    kj::StringPtr rawSpecifier;
    kj::StringPtr referrer;
    kj::StringPtr attributeType;
  };
  Case cases[] = {
    {"file:///bundle/a.js"_kj, "./a.js"_kj, "file:///bundle/worker.js"_kj, "json"_kj},
    {"file:///bundle/a.js"_kj, ""_kj, "file:///bundle/worker.js"_kj, ""_kj},
    {"file:///\"quoted\"\\back"_kj, "\b\f\n\r\t"_kj, "\x01\x1f\x7f"_kj, "text"_kj},
    {"file:///caf\xc3\xa9/\xff/\xed\xa0\x80"_kj, "/"_kj, "</script>"_kj, "\xe2\x82\xac"_kj},
  };
  for (auto& c: cases) {
    auto cpp = cppV2(c.specifier, c.rawSpecifier, c.referrer, c.attributeType);
    auto rust =
        encode_request(true, c.specifier.asBytes().as<Rust>(), c.rawSpecifier.asBytes().as<Rust>(),
            c.referrer.asBytes().as<Rust>(), c.attributeType.asBytes().as<Rust>());
    KJ_EXPECT(kj::from<Rust>(rust.body) == cpp.asBytes(), cpp,
        kj::str(kj::from<Rust>(rust.body).asChars()));
    KJ_EXPECT(kj::from<Rust>(rust.expected_name) == c.specifier.asBytes());
  }
}

KJ_TEST("modules decode as JsonCodec decodes them") {
  // Each body is decoded for the module "a"; some fail in both.
  kj::ArrayPtr<const char> bodies[] = {
    // Every module type.
    R"({"esModule":"export default 1;"})"_kj,
    R"({"commonJsModule":"module.exports = 1","namedExports":["a","b"]})"_kj,
    R"({"text":"t"})"_kj,
    R"({"data":[0,1,127,255]})"_kj,
    R"({"wasm":[0,97,115,109,1,0,0,0]})"_kj,
    R"({"json":"{\"answer\":42}"})"_kj,
    R"({"obsolete":"o"})"_kj,
    R"({"pythonModule":"x = 1"})"_kj,
    R"({"obsoletePythonRequirement":"r"})"_kj,
    R"({"bundled":"b"})"_kj,
    R"({})"_kj,
    // Names.
    R"({"name":"a","text":"t"})"_kj,
    R"({"name":"b","text":"t"})"_kj,
    R"({"name":"","text":"t"})"_kj,
    R"({"name":"b","name":"a","text":"t"})"_kj,
    R"({"name":null,"text":"t"})"_kj,
    // Union members, unknown members and nulls.
    R"({"text":"t","esModule":"e"})"_kj,
    R"({"esModule":"e","text":"t"})"_kj,
    R"({"esModule":"e","text":null})"_kj,
    R"({"future":{"nested":[1,{"x":null}]},"text":"t"})"_kj,
    R"({"namedExports":null,"commonJsModule":"c"})"_kj,
    // Data.
    R"({"data":[1.0,2e0,-0,1.,1.e1]})"_kj,
    R"({"data":[256]})"_kj,
    R"({"data":[-1]})"_kj,
    R"({"data":[1.5]})"_kj,
    R"({"data":["1"]})"_kj,
    R"({"data":[null]})"_kj,
    R"({"data":"AAEC"})"_kj,
    R"({"data":[]})"_kj,
    // Wrong kinds.
    R"({"text":1})"_kj,
    R"({"text":true})"_kj,
    R"({"text":["t"]})"_kj,
    R"({"namedExports":"a"})"_kj,
    R"({"namedExports":[1]})"_kj,
    R"({"namedExports":[null]})"_kj,
    R"([])"_kj,
    R"("text")"_kj,
    R"(null)"_kj,
    R"(1)"_kj,
    // Strings.
    R"({"text":"Aé€\ud800😀\u0000"})"_kj,
    R"({"text":"\"\\\/\b\f\n\r\t"})"_kj,
    "{\"text\":\"\xff\xfe raw\"}"_kj,
    "{\"text\":\"tab\there\x01\"}"_kj,
    R"({"text":"\u12g4"})"_kj,
    R"({"text":"\u12"})"_kj,
    R"({"text":"\x"})"_kj,
    R"({"text":"unterminated)"_kj,
    // Syntax.
    " \t\r\n{ \"text\" : \"t\" } \t\r\n"_kj,
    "\x0c{\"text\":\"t\"}"_kj,
    R"({"text":"t"} {})"_kj,
    R"({"text":"t",})"_kj,
    R"({"text":"t" "esModule":"e"})"_kj,
    R"({text:"t"})"_kj,
    R"({"a":[1,]})"_kj,
    R"({"a":[1 2]})"_kj,
    R"({"a":01})"_kj,
    R"({"a":1e})"_kj,
    R"({"a":-})"_kj,
    R"({"a":.5})"_kj,
    R"({"a":1e400})"_kj,
    R"({"a":nul})"_kj,
    R"({)"_kj,
    ""_kj,
    "   "_kj,
  };
  for (auto body: bodies) {
    auto cpp = cppDecode(body, "a"_kj);
    auto rust = rustDecode(body, "a"_kj);
    KJ_IF_SOME(c, cpp) {
      KJ_IF_SOME(r, rust) {
        KJ_EXPECT(c.asBytes() == r.asBytes(), ptr(body));
      } else {
        KJ_FAIL_EXPECT("only C++ decoded it", ptr(body));
      }
    } else {
      KJ_EXPECT(rust == kj::none, "only Rust decoded it", ptr(body));
    }
  }

  // Bodies with NUL bytes, and nesting.
  struct Raw {
    kj::ArrayPtr<const char> body;
  };
  auto deep = [](size_t n) {
    return kj::str(
        "{\"future\":", kj::repeat('[', n - 1), kj::repeat(']', n - 1), ",\"text\":\"t\"}");
  };
  auto deep64 = deep(64);
  auto deep65 = deep(65);
  static constexpr char nulTrailing[] = "{\"text\":\"t\"}\0garbage";
  static constexpr char nulInString[] = "{\"text\":\"a\0b\"}";
  static constexpr char nulFirst[] = "\0{\"text\":\"t\"}";
  Raw raws[] = {
    {kj::arrayPtr(nulTrailing, sizeof(nulTrailing) - 1)},
    {kj::arrayPtr(nulInString, sizeof(nulInString) - 1)},
    {kj::arrayPtr(nulFirst, sizeof(nulFirst) - 1)},
    {deep64.asArray()},
    {deep65.asArray()},
  };
  for (auto& raw: raws) {
    auto cpp = cppDecode(raw.body, "a"_kj);
    auto rust = rustDecode(raw.body, "a"_kj);
    KJ_EXPECT((cpp == kj::none) == (rust == kj::none), raw.body.size());
    KJ_IF_SOME(c, cpp) {
      KJ_IF_SOME(r, rust) {
        KJ_EXPECT(c.asBytes() == r.asBytes(), raw.body.size());
      }
    }
  }
}

}  // namespace
}  // namespace workerd::fallback::test
