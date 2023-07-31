// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"
#include "string.h"
#include <algorithm>

namespace workerd::jsg::test {
namespace {

KJ_TEST("UsvString from kj::String") {
  {
    auto kjStr = kj::str(u8"\U0001F607hello");
    auto usvStr = usv(kjStr.asPtr());

    KJ_ASSERT(usvStr.size(), 6);

    KJ_ASSERT(usvStr.getCodepointAt(0), 128519);
    KJ_ASSERT(usvStr.getCodepointAt(1), 'h');
    KJ_ASSERT(usvStr.getCodepointAt(2), 'e');
    KJ_ASSERT(usvStr.getCodepointAt(3), 'l');
    KJ_ASSERT(usvStr.getCodepointAt(4), 'l');
    KJ_ASSERT(usvStr.getCodepointAt(5), 'o');

    {
      auto end = usvStr.end();

      auto n = 0;
      for (auto it = usvStr.begin(); it < end; ++it, n++) {
        KJ_ASSERT(it.position() == n);
        KJ_ASSERT(*it == usvStr.getCodepointAt(n));
      }

      n = 0;
      for (auto it = usvStr.begin(); it != end; ++it, n++) {
        KJ_ASSERT(*it == usvStr.getCodepointAt(n));
      }

      n = 0;
      for (auto it = usvStr.begin(); it < end; it++, n++) {
        KJ_ASSERT(*it == usvStr.getCodepointAt(n));
      }

      n = 0;
      for (auto it = usvStr.begin(); it != end; it++, n++) {
        KJ_ASSERT(*it == usvStr.getCodepointAt(n));
      }
    }

    {
      auto it = usvStr.begin();
      auto n = 0;
      while (it) {
        KJ_ASSERT(*it == usvStr.getCodepointAt(n));
        it++;
        n++;
      }
      KJ_ASSERT(it.position() == it.size());
      KJ_ASSERT(n == usvStr.size());
    }

    {
      auto it = usvStr.begin();
      auto n = 0;
      while (it) {
        KJ_ASSERT(*it == usvStr.getCodepointAt(n));
        ++it;
        n++;
      }
      KJ_ASSERT(n == usvStr.size());
    }

    {
      auto it = usvStr.begin();
      auto n = 0;
      while (it) {
        KJ_ASSERT(*it == usvStr.getCodepointAt(n));
        it += 3;
        n += 3;
      }
      KJ_ASSERT(n == usvStr.size());
    }

    KJ_ASSERT(usvStr.toStr() == u8"😇hello");
  }

  {
    UsvStringBuilder builder;
    builder.add('a');
    builder.add('b');
    builder.add('c');
    builder.add(0x10ffff); // Adds a surrogate pair

    KJ_ASSERT(builder.size() == 4);

    auto s = builder.finish();
    auto it = s.begin();

    KJ_ASSERT(*it == 'a');
    KJ_ASSERT(*(it + 1) == 'b');
    KJ_ASSERT(*(it + 2) == 'c');
    KJ_ASSERT(*(it + 3) == 0x10ffff);
  }

  {
    UsvStringBuilder builder;
    builder.add('a', 'b', 'c', 0x10ffff);
    KJ_ASSERT(builder.size() == 4);

    auto s = builder.finish();
    auto it = s.begin();

    KJ_ASSERT(*it == 'a');
    KJ_ASSERT(*(it + 1) == 'b');
    KJ_ASSERT(*(it + 2) == 'c');
    KJ_ASSERT(*(it + 3) == 0x10ffff);
  }

  {
    auto usvStr = usv("abc");
    UsvStringBuilder builder;
    builder.addAll(usvStr.begin(), usvStr.end());
    KJ_ASSERT(builder.size() == 3);
  }

  {
    UsvStringBuilder builder;
    builder.addAll(usv("abc"));
    KJ_ASSERT(builder.size() == 3);
  }

  {
    UsvStringBuilder builder(10);
    KJ_ASSERT(builder.capacity() == 10);
  }

  {
    auto usvStr = usv("hëllo"_kj);
    {
      auto ptr = usvStr.slice(2, 4);
      KJ_ASSERT(ptr.size() == 2);
      KJ_ASSERT(ptr.toStr() == "ll");
    }
    {
      auto ptr = usvStr.slice(1, 2);
      KJ_ASSERT(ptr.size() == 1);
      KJ_ASSERT(ptr.toStr() == u8"ë");
    }
    {
      auto ptr = usvStr.slice(1, 1);
      KJ_ASSERT(ptr.size() == 0);
      KJ_ASSERT(ptr.toStr() == "");
    }
    {
      auto ptr = usvStr.slice(1, 5);
      KJ_ASSERT(ptr.size() == 4);
      KJ_ASSERT(ptr.toStr() == u8"ëllo");
    }
  }

  {
    UsvString str;
    KJ_ASSERT(str.size() == 0);
    KJ_ASSERT(str.storage().size() == 0);
    KJ_ASSERT(str.toStr() == "");
  }

  {
    auto usvStr = usv("something"_kj);
    KJ_ASSERT(usvStr.toStr() == "something");
  }

  {
    auto first = usv("something");
    auto second = first.clone();
    KJ_ASSERT(second.toStr() == "something");

    auto third = first.asPtr().clone();
    KJ_ASSERT(third.toStr() == "something");

    auto fourth = usv(first.asPtr());
    KJ_ASSERT(kj::str(fourth) == "something");

    auto fifth = usv(first);
    KJ_ASSERT(kj::str(fifth) == "something");
  }

  {
    auto usvStr = usv('a', 'b', 'c');
    KJ_ASSERT(usvStr.size() == 3);
    KJ_ASSERT(usvStr.toStr() == "abc");
  }

  {
    UsvString one;
    KJ_ASSERT(one.toStr() == "");
    KJ_ASSERT(one.size() == 0);
    UsvString two = usv("two");
    one = kj::mv(two);
    KJ_ASSERT(one.toStr() == "two");
    KJ_ASSERT(one.size() == 3);
    KJ_ASSERT(two.toStr() == "");
    KJ_ASSERT(two.size() == 0);
  }

  {
    UsvString str = usv("abc");
    auto ptr1 = str.asPtr();
    KJ_ASSERT(ptr1.size() == 3);
    KJ_ASSERT(ptr1 == usv("abc"));
    auto ptr2 = ptr1;
    KJ_ASSERT(ptr2.size() == 3);
    KJ_ASSERT(ptr2 == usv("abc"));
    KJ_ASSERT(ptr1.size() == 3);
    KJ_ASSERT(ptr1 == usv("abc"));
  }

  {
    UsvString str = usv("abc");
    auto ptr1 = str.asPtr();
    KJ_ASSERT(ptr1.size() == 3);
    KJ_ASSERT(ptr1 == usv("abc"));
    UsvStringPtr ptr2(ptr1);
    KJ_ASSERT(ptr2.size() == 3);
    KJ_ASSERT(ptr2 == usv("abc"));
    KJ_ASSERT(ptr1.size() == 3);
    KJ_ASSERT(ptr1 == usv("abc"));
  }

  {
    UsvString str = usv("abc");
    auto ptr1 = str.asPtr();
    KJ_ASSERT(ptr1.size() == 3);
    KJ_ASSERT(ptr1 == usv("abc"));
    auto ptr2 = kj::mv(ptr1);
    KJ_ASSERT(ptr2.size() == 3);
    KJ_ASSERT(ptr2 == usv("abc"));
    KJ_ASSERT(ptr1.size() == 3);
    KJ_ASSERT(ptr1 == usv("abc"));
  }

  {
    auto str = usv("abc");
    auto ptr1 = str.asPtr();
    KJ_ASSERT(ptr1.size() == 3);
    KJ_ASSERT(ptr1 == usv("abc"));
    UsvStringPtr ptr2(kj::mv(ptr1));
    KJ_ASSERT(ptr2.size() == 3);
    KJ_ASSERT(ptr2 == usv("abc"));
    KJ_ASSERT(ptr1.size() == 3);
    KJ_ASSERT(ptr1 == usv("abc"));
  }

  {
    auto one = usv("str");
    auto two = usv("str");
    auto three = usv("abc");
    KJ_ASSERT(one == two);
    KJ_ASSERT(one != three);
    KJ_ASSERT(one.asPtr() == two.asPtr());
    KJ_ASSERT(one.asPtr() != three.asPtr());
    KJ_ASSERT(one == two.asPtr());
    KJ_ASSERT(one != three.asPtr());
    KJ_ASSERT(one.asPtr() == two);
    KJ_ASSERT(one.asPtr() != three);
  }

  {
    auto data = kj::arr(usv("café"_kj),
                        usv("Café"_kj),
                        usv("cafe"_kj),
                        usv("Cafe"_kj),
                        usv("a"_kj));
    std::sort(data.begin(), data.end());
    KJ_ASSERT(data[0] == usv("Cafe"_kj));
    KJ_ASSERT(data[1] == usv("Café"_kj));
    KJ_ASSERT(data[2] == usv("a"_kj));
    KJ_ASSERT(data[3] == usv("cafe"_kj));
    KJ_ASSERT(data[4] == usv("café"_kj));

    auto str1 = usv("Café"_kj);
    auto str2 = usv("Cafe"_kj);

    KJ_ASSERT(usv("Cafe"_kj) < usv("Café"_kj));
    KJ_ASSERT(usv("Cafe"_kj) < str1.asPtr());
    KJ_ASSERT(usv("Cafe"_kj) <= usv("Cafe"_kj));
    KJ_ASSERT(usv("Cafe"_kj) <= usv("Café"_kj));
    KJ_ASSERT(usv("Café"_kj) <= usv("Café"_kj));
    KJ_ASSERT(usv("Cafe"_kj) <= str1.asPtr());

    KJ_ASSERT(usv("Café"_kj) > usv("Cafe"_kj));
    KJ_ASSERT(usv("Café"_kj) > str2.asPtr());
    KJ_ASSERT(usv("Café"_kj) >= usv("Cafe"_kj));
    KJ_ASSERT(usv("Cafe"_kj) >= usv("Cafe"_kj));
    KJ_ASSERT(usv("Café"_kj) >= usv("Café"_kj));
    KJ_ASSERT(usv("Café"_kj) >= str1.asPtr());
  }

  {
    auto str1 = usv("café"_kj);
    auto str2 = usv("Café"_kj);
    auto str3 = usv("cafe"_kj);
    auto str4 = usv("Cafe"_kj);
    auto str5 = usv("a"_kj);
    auto data = kj::arr(str1.asPtr(), str2.asPtr(), str3.asPtr(), str4.asPtr(), str5.asPtr());
    std::sort(data.begin(), data.end());
    KJ_ASSERT(data[0] == usv("Cafe"_kj));
    KJ_ASSERT(data[1] == usv("Café"_kj));
    KJ_ASSERT(data[2] == usv("a"_kj));
    KJ_ASSERT(data[3] == usv("cafe"_kj));
    KJ_ASSERT(data[4] == usv("café"_kj));

    KJ_ASSERT(usv("Cafe"_kj).asPtr() < str2.asPtr());
    KJ_ASSERT(usv("Cafe"_kj).asPtr() < usv("Café"_kj));
    KJ_ASSERT(usv("Cafe"_kj).asPtr() <= str2.asPtr());
    KJ_ASSERT(usv("Cafe"_kj).asPtr() <= usv("Café"_kj));
    KJ_ASSERT(usv("Cafe"_kj).asPtr() <= usv("Cafe"_kj));

    KJ_ASSERT(usv("Café"_kj).asPtr() > str4.asPtr());
    KJ_ASSERT(usv("Café"_kj).asPtr() > usv("Cafe"_kj));
    KJ_ASSERT(usv("Café"_kj).asPtr() >= str4.asPtr());
    KJ_ASSERT(usv("Café"_kj).asPtr() >= usv("Cafe"_kj));
    KJ_ASSERT(usv("Café"_kj).asPtr() >= usv("Café"_kj));
  }

  {
    // Unpaired surrogates get transformed consistently.
    auto str1 = usv("\xd8\x00");
    auto str2 = usv("\xd8\x01");
    KJ_ASSERT(memcmp(str1.storage().begin(), str2.storage().begin(), 4) == 0);
  }

  {
    auto str = usv("abc:xyz:123");
    KJ_ASSERT(str.lastIndexOf(':') == 7);
    KJ_ASSERT(str.asPtr().lastIndexOf(':') == 7);
    KJ_ASSERT(str.lastIndexOf('#') == nullptr);
    KJ_ASSERT(str.asPtr().lastIndexOf('#') == nullptr);

    auto str2 = usv("#abc:xyz:123");
    KJ_ASSERT(str2.lastIndexOf('#') == 0);
    KJ_ASSERT(str2.asPtr().lastIndexOf('#') == 0);

    auto str3 = usv("abc:xyz:123#");
    KJ_ASSERT(str3.lastIndexOf('#') == 11);
    KJ_ASSERT(str3.asPtr().lastIndexOf('#') == 11);
  }
}

V8System v8System;

struct UsvStringContext: public jsg::Object, public jsg::ContextGlobal {
  UsvString testUsv(jsg::Optional<UsvString> str) {
    return kj::mv(str).orDefault(usv("undefined"));
  }

  UsvStringPtr testUsvPtr(jsg::Optional<UsvString> str) {
    return kj::mv(str).orDefault(usv("undefined"));
  }

  JSG_RESOURCE_TYPE(UsvStringContext) {
    JSG_METHOD(testUsv);
    JSG_METHOD(testUsvPtr);
  }
};
JSG_DECLARE_ISOLATE_TYPE(UsvStringIsolate, UsvStringContext);

KJ_TEST("JavaScript USVStrings") {
  Evaluator<UsvStringContext, UsvStringIsolate> e(v8System);

  e.expectEval("testUsv('hello')", "string", "hello");
  e.expectEval("testUsvPtr('hello')", "string", "hello");
  e.expectEval(u8"testUsv('hello\xd8')", "string", u8"hello�");
  e.expectEval("testUsv(1)", "string", "1");
  e.expectEval("testUsv(false)", "string", "false");
  e.expectEval("testUsv({})", "string", "[object Object]");
  e.expectEval("testUsv(undefined)", "string", "undefined");
  e.expectEval("testUsv()", "string", "undefined");
  e.expectEval("testUsv(null)", "string", "null");
  e.expectEval("testUsv({ toString() { return 1; } })", "string", "1");
  e.expectEval("testUsv(Symbol('foo'))", "throws",
               "TypeError: Cannot convert a Symbol value to a string");
  e.expectEval("testUsv('\\ud999') === '\\ufffd'", "boolean", "true");
  e.expectEval("testUsv('\\ud800blank') === '\\ufffdblank'", "boolean", "true");
  e.expectEval("testUsv('\\uda99') === '\\ufffd'", "boolean", "true");
  e.expectEval("testUsv('\\uda99\\uda99') === '\\ufffd'.repeat(2)", "boolean", "true");
  e.expectEval("testUsv('\\ud800\\ud800') === '\\ufffd'.repeat(2)", "boolean", "true");
}

}  // namespace
}  // namespace::jsg::test
