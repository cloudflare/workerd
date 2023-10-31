// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sqlite-kv.h"
#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("SQLite-KV") {
  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);
  SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
  SqliteKv kv(db);

  kv.put("foo", "abc"_kj.asBytes());
  kv.put("bar", "def"_kj.asBytes());
  kv.put("baz", "123"_kj.asBytes());
  kv.put("qux", "321"_kj.asBytes());

  {
    bool called = false;
    KJ_EXPECT(kv.get("foo", [&](kj::ArrayPtr<const byte> value) {
      KJ_EXPECT(kj::str(value.asChars()) == "abc");
      called = true;
    }));
    KJ_EXPECT(called);
  }

  {
    bool called = false;
    KJ_EXPECT(kv.get("bar", [&](kj::ArrayPtr<const byte> value) {
      KJ_EXPECT(kj::str(value.asChars()) == "def");
      called = true;
    }));
    KJ_EXPECT(called);
  }

  KJ_EXPECT(!kv.get("corge", [&](kj::ArrayPtr<const byte> value) {
    KJ_FAIL_EXPECT("should not call callback when no match", value.asChars());
  }));

  auto list = [&](auto&&... params) {
    kj::Vector<kj::String> results;
    auto callback = [&](kj::StringPtr key, kj::ArrayPtr<const byte> value) {
      results.add(kj::str(key, "=", value.asChars()));
    };

    auto n = kv.list(params..., callback);
    KJ_EXPECT(results.size() == n);
    return kj::strArray(results, ", ");
  };

  constexpr auto F = SqliteKv::FORWARD;
  constexpr auto R = SqliteKv::REVERSE;

  KJ_EXPECT(list(nullptr, kj::none, kj::none, F) == "bar=def, baz=123, foo=abc, qux=321");
  KJ_EXPECT(list("cat"_kj, kj::none, kj::none, F) == "foo=abc, qux=321");
  KJ_EXPECT(list("foo"_kj, kj::none, kj::none, F) == "foo=abc, qux=321");
  KJ_EXPECT(list("fop"_kj, kj::none, kj::none, F) == "qux=321");
  KJ_EXPECT(list("foo "_kj, kj::none, kj::none, F) == "qux=321");

  KJ_EXPECT(list(nullptr, "cat"_kj, kj::none, F) == "bar=def, baz=123");
  KJ_EXPECT(list(nullptr, "foo"_kj, kj::none, F) == "bar=def, baz=123");
  KJ_EXPECT(list(nullptr, "fop"_kj, kj::none, F) == "bar=def, baz=123, foo=abc");

  KJ_EXPECT(list(nullptr, kj::none, 2, F) == "bar=def, baz=123");
  KJ_EXPECT(list(nullptr, kj::none, 3, F) == "bar=def, baz=123, foo=abc");
  KJ_EXPECT(list("baz"_kj, kj::none, 2, F) == "baz=123, foo=abc");
  KJ_EXPECT(list(nullptr, "foo"_kj, 1, F) == "bar=def");
  KJ_EXPECT(list(nullptr, "foo"_kj, 2, F) == "bar=def, baz=123");
  KJ_EXPECT(list(nullptr, "foo"_kj, 3, F) == "bar=def, baz=123");

  KJ_EXPECT(list(nullptr, kj::none, kj::none, R) == "qux=321, foo=abc, baz=123, bar=def");
  KJ_EXPECT(list("foo"_kj, kj::none, kj::none, R) == "qux=321, foo=abc");
  KJ_EXPECT(list(nullptr, "foo"_kj, kj::none, R) == "baz=123, bar=def");
  KJ_EXPECT(list(nullptr, kj::none, 2, R) == "qux=321, foo=abc");
  KJ_EXPECT(list(nullptr, "foo"_kj, 1, R) == "baz=123");

  KJ_EXPECT(kv.delete_("baz"));
  KJ_EXPECT(!kv.delete_("corge"));

  KJ_EXPECT(list(nullptr, kj::none, kj::none, F) == "bar=def, foo=abc, qux=321");

  // Put can overwrite.
  kv.put("foo", "hello"_kj.asBytes());
  KJ_EXPECT(list(nullptr, kj::none, kj::none, F) == "bar=def, foo=hello, qux=321");

  kv.deleteAll();
  KJ_EXPECT(list(nullptr, kj::none, kj::none, F) == "");
}

}  // namespace
}  // namespace workerd
