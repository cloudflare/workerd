// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A Headers object built from a kj::HttpHeaders holds a snapshot of it until JavaScript changes
// it, rather than the maps every other Headers object keeps. These tests check that it is
// indistinguishable from one that built the maps right away, as the constructor used to.

#include <workerd/api/headers.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

#include <random>

namespace workerd::api {
namespace {

// Header names to draw from: names kj's builtin table has, common names it may not, uncommon
// names, names with non-ASCII bytes, and set-cookie, which kj and Headers both treat specially.
const kj::StringPtr NAMES[] = {
  "Host"_kj,
  "Content-Type"_kj,
  "Content-Length"_kj,
  "Transfer-Encoding"_kj,
  "Accept"_kj,
  "Accept-Encoding"_kj,
  "Accept-Language"_kj,
  "Cache-Control"_kj,
  "Cookie"_kj,
  "Set-Cookie"_kj,
  "User-Agent"_kj,
  "Referer"_kj,
  "Vary"_kj,
  "WWW-Authenticate"_kj,
  "X-Forwarded-For"_kj,
  "CF-Connecting-IP"_kj,
  "CF-Ray"_kj,
  "Sec-Ch-Ua"_kj,
  "Sec-Fetch-Mode"_kj,
  "Priority"_kj,
  "X-Custom"_kj,
  "x-a"_kj,
  "A"_kj,
  "X-\xc1z"_kj,
  "X-\xc1Z"_kj,
  "\xe9t\xe9"_kj,
};

// The names Headers keeps as common headers, which a workerd table indexes; see
// http-over-capnp.capnp.
const kj::StringPtr COMMON_NAMES[] = {
  "Accept-Charset"_kj,
  "Accept-Encoding"_kj,
  "Accept-Language"_kj,
  "Accept-Ranges"_kj,
  "Accept"_kj,
  "Access-Control-Allow-Origin"_kj,
  "Age"_kj,
  "Allow"_kj,
  "Authorization"_kj,
  "Cache-Control"_kj,
  "Content-Disposition"_kj,
  "Content-Encoding"_kj,
  "Content-Language"_kj,
  "Content-Length"_kj,
  "Content-Location"_kj,
  "Content-Range"_kj,
  "Content-Type"_kj,
  "Cookie"_kj,
  "Date"_kj,
  "ETag"_kj,
  "Expect"_kj,
  "Expires"_kj,
  "From"_kj,
  "Host"_kj,
  "If-Match"_kj,
  "If-Modified-Since"_kj,
  "If-None-Match"_kj,
  "If-Range"_kj,
  "If-Unmodified-Since"_kj,
  "Last-Modified"_kj,
  "Link"_kj,
  "Location"_kj,
  "Max-Forwards"_kj,
  "Proxy-Authenticate"_kj,
  "Proxy-Authorization"_kj,
  "Range"_kj,
  "Referer"_kj,
  "Refresh"_kj,
  "Retry-After"_kj,
  "Server"_kj,
  "Set-Cookie"_kj,
  "Strict-Transport-Security"_kj,
  "Transfer-Encoding"_kj,
  "User-Agent"_kj,
  "Vary"_kj,
  "Via"_kj,
  "WWW-Authenticate"_kj,
};

struct Random {
  std::mt19937 engine;

  explicit Random(uint32_t seed): engine(seed) {}

  size_t below(size_t n) {
    return std::uniform_int_distribution<size_t>(0, n - 1)(engine);
  }

  // `name` with the case of each ASCII letter picked at random.
  kj::String recase(kj::StringPtr name) {
    auto result = kj::heapString(name);
    for (char& c: result) {
      if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z')) {
        c = below(2) ? (c | 0x20) : (c & ~0x20);
      }
    }
    return result;
  }

  kj::String value() {
    static constexpr kj::StringPtr PIECES[] = {
      ""_kj, "a"_kj, "B"_kj, "text/html"_kj, "x=1; Path=/"_kj, " spaced "_kj, "\xc3\xa9"_kj};
    auto size = below(3);
    kj::Vector<kj::StringPtr> parts;
    for (auto i KJ_UNUSED: kj::zeroTo(size)) parts.add(PIECES[below(kj::size(PIECES))]);
    return kj::strArray(parts, "");
  }
};

// Builds a Headers object the way the constructor did before snapshots: by appending every
// header of `headers` to the maps.
jsg::Ref<Headers> eagerFrom(jsg::Lock& js, const kj::HttpHeaders& headers) {
  auto result = js.alloc<Headers>();
  headers.forEach([&](kj::StringPtr name, kj::StringPtr value) {
    result->appendUnguarded(js, kj::str(name), kj::str(value));
  });
  return result;
}

kj::String show(kj::Maybe<kj::String> value) {
  KJ_IF_SOME(v, value) {
    return kj::str("[", v, "]");
  }
  return kj::str("none");
}

kj::String copiedToKj(const kj::HttpHeaderTable& table, Headers& headers) {
  kj::HttpHeaders out(table);
  headers.shallowCopyTo(out);
  return out.toString();
}

kj::String displayed(jsg::Lock& js, Headers& headers) {
  return kj::strArray(KJ_MAP(h, headers.getDisplayedHeaders(js)) {
    return kj::str(h.key, ": ", h.value);
  }, "\n");
}

// Every read of `a` and `b` that doesn't change them gives the same answer.
void expectSameReads(jsg::Lock& js,
    const kj::HttpHeaderTable& table,
    Random& random,
    Headers& a,
    Headers& b,
    kj::StringPtr context) {
  auto check = [&](kj::StringPtr name) {
    KJ_EXPECT(show(a.getPtr(js, name)) == show(b.getPtr(js, name)), name, context);
    KJ_EXPECT(a.has(kj::str(name)) == b.has(kj::str(name)), name, context);
  };
  for (auto name: NAMES) {
    check(name);
    check(random.recase(name));
  }
  for (auto name: COMMON_NAMES) check(name);
  check("not-a-header"_kj);
  check(""_kj);

  for (auto id: kj::range<uint>(1, Headers::MAX_COMMON_HEADER_ID + 1)) {
    auto common = static_cast<capnp::CommonHeaderName>(id);
    KJ_EXPECT(show(a.getCommon(js, common)) == show(b.getCommon(js, common)), id, context);
    KJ_EXPECT(a.hasCommon(common) == b.hasCommon(common), id, context);
  }

  KJ_EXPECT(kj::strArray(a.getSetCookie(), "|") == kj::strArray(b.getSetCookie(), "|"), context);
  KJ_EXPECT(displayed(js, a) == displayed(js, b), context);
  KJ_EXPECT(copiedToKj(table, a) == copiedToKj(table, b), context);
}

// Random header sets, each built into a kj::HttpHeaders on `table`, as a Headers object from the
// snapshot and from the maps.
void checkTable(jsg::Lock& js, const kj::HttpHeaderTable& table, uint32_t seed) {
  Random random(seed);
  for (auto round: kj::zeroTo(400)) {
    kj::Vector<kj::String> owned;
    kj::HttpHeaders kjHeaders(table);
    // Mostly small sets, sometimes more than a snapshot takes.
    auto count = random.below(8) == 0 ? random.below(90) : random.below(20);
    for (auto i KJ_UNUSED: kj::zeroTo(count)) {
      auto name = random.recase(NAMES[random.below(kj::size(NAMES))]);
      auto value = random.value();
      kjHeaders.addPtrPtr(name, value);
      owned.add(kj::mv(name));
      owned.add(kj::mv(value));
    }
    auto context = kj::str("seed ", seed, " round ", round, ":\n", kjHeaders.toString());

    auto lazy = js.alloc<Headers>(js, kjHeaders, Headers::Guard::NONE);
    auto eager = eagerFrom(js, kjHeaders);
    KJ_EXPECT(!eager->holdsSnapshot());
    if (kjHeaders.size() > 0 && kjHeaders.size() <= 64) {
      KJ_EXPECT(lazy->holdsSnapshot(), context);
    }
    expectSameReads(js, table, random, *lazy, *eager, context);
    // Reads leave the snapshot in place.
    KJ_EXPECT(lazy->holdsSnapshot() == (kjHeaders.size() > 0 && kjHeaders.size() <= 64));

    // A clone shares the snapshot and reads the same.
    auto lazyClone = lazy->clone(js);
    KJ_EXPECT(lazyClone->holdsSnapshot() == lazy->holdsSnapshot());
    expectSameReads(js, table, random, *lazyClone, *eager, context);

    // Change the clone and a copy of the eager one the same way: the snapshot-backed one builds
    // its maps and still matches, and the object it was cloned from is untouched.
    auto eagerClone = eager->clone(js);
    auto nameToChange = random.recase(NAMES[random.below(kj::size(NAMES))]);
    switch (random.below(3)) {
      case 0:
        lazyClone->set(js, kj::str(nameToChange), kj::str("changed"));
        eagerClone->set(js, kj::str(nameToChange), kj::str("changed"));
        break;
      case 1:
        lazyClone->append(js, kj::str(nameToChange), kj::str("appended"));
        eagerClone->append(js, kj::str(nameToChange), kj::str("appended"));
        break;
      case 2:
        lazyClone->delete_(kj::str(nameToChange));
        eagerClone->delete_(kj::str(nameToChange));
        break;
    }
    KJ_EXPECT(!lazyClone->holdsSnapshot());
    expectSameReads(js, table, random, *lazyClone, *eagerClone, context);
    expectSameReads(js, table, random, *lazy, *eager, context);
  }
}

KJ_TEST("Headers built from a kj::HttpHeaders read as if built from its maps") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    // kj's table only, so common names other than kj's builtins arrive unindexed, duplicates
    // and all.
    kj::HttpHeaderTable builtinOnly;
    checkTable(env.js, builtinOnly, 1);

    // A table that indexes every common name, as workerd's does.
    kj::HttpHeaderTable::Builder builder;
    for (auto name: COMMON_NAMES) builder.add(name);
    builder.add("CF-Ray");
    auto indexed = builder.build();
    checkTable(env.js, *indexed, 2);
  });
}

KJ_TEST("Headers keeps a snapshot's strings alive after building its maps") {
  // shallowCopyTo() hands out pointers to the snapshot's strings. Changing one header must
  // leave the others valid, as it did when each header owned its strings.
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    kj::HttpHeaderTable table;
    kj::String expected;
    kj::HttpHeaders out(table);
    auto headers = [&]() {
      auto name = kj::str("X-Kept");
      auto value = kj::str("kept value");
      kj::HttpHeaders in(table);
      in.addPtrPtr(name, value);
      in.addPtrPtr("Content-Type", "text/plain");
      // `in` and its strings go away on return: the Headers object must not depend on them.
      return js.alloc<Headers>(js, in, Headers::Guard::NONE);
    }();
    KJ_EXPECT(headers->holdsSnapshot());
    headers->shallowCopyTo(out);
    expected = out.toString();
    headers->set(js, kj::str("X-Other"), kj::str("new"));
    KJ_EXPECT(!headers->holdsSnapshot());
    KJ_EXPECT(out.toString() == expected);
    KJ_EXPECT(show(headers->getPtr(js, "x-kept"_kj)) == "[kept value]");
    KJ_EXPECT(show(headers->getPtr(js, "x-other"_kj)) == "[new]");
  });
}

KJ_TEST("Headers looks up an invalid name in a snapshot as it would in its maps") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    kj::HttpHeaderTable table;
    kj::HttpHeaders in(table);
    in.addPtrPtr("X-A", "1");
    auto lazy = js.alloc<Headers>(js, in, Headers::Guard::NONE);
    auto eager = eagerFrom(js, in);
    KJ_EXPECT(lazy->holdsSnapshot());
    for (auto bad: {"bad name"_kj, "x(y)"_kj, "a:b"_kj}) {
      KJ_EXPECT_THROW_MESSAGE("Invalid header name.", lazy->getPtr(js, bad));
      KJ_EXPECT_THROW_MESSAGE("Invalid header name.", eager->getPtr(js, bad));
      KJ_EXPECT_THROW_MESSAGE("Invalid header name.", lazy->has(kj::str(bad)));
      KJ_EXPECT_THROW_MESSAGE("Invalid header name.", eager->has(kj::str(bad)));
    }
  });
}

}  // namespace
}  // namespace workerd::api
