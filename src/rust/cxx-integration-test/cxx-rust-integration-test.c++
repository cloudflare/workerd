#include <rust/cxx-integration-test/lib.rs.h>
#include <rust/cxx-integration/lib.rs.h>
#include <rust/cxx.h>

#include <signal.h>

#include <kj/async.h>
#include <kj/test.h>

// Test generic rust/c++ integration boundary.
// See src/rust/cxx-integration-tests for rust backend.

namespace workerd::rust {

KJ_TEST("init cxx_integration") {
  // this tests initializes cxx integration for the rest of the tests
  rust::cxx_integration::init();
}

KJ_TEST("panic results in abort") {
  KJ_EXPECT_SIGNAL(SIGABRT, rust::cxx_integration::trigger_panic("foobar"));
}

KJ_TEST("ok Result") { KJ_EXPECT(42 == rust::test::result_ok()); }

KJ_TEST("err Result") {
  // if fn returns an error, it is translated into ::rust::Error exception.
  try {
    rust::test::result_error();
    KJ_FAIL_REQUIRE("exception is expected");
  } catch (::rust::Error e) {
    // this is expected
    KJ_EXPECT(e.what() == "test error"_kj);
  }
}

KJ_TEST("test callback") {
  rust::test::TestCallback callback = [](size_t a, size_t b) { return a + b; };
  auto result = rust::test::call_callback(callback, 40, 2);
  KJ_EXPECT(result == 42);
}

KJ_TEST("test crashing callback") {
  rust::test::TestCallback callback = [](size_t a, size_t b) -> size_t {
    KJ_FAIL_REQUIRE("expected to crash");
  };
  // std::terminate is called when c++ throws fatal except
  KJ_EXPECT_SIGNAL(SIGABRT, rust::test::call_callback(callback, 40, 2));
}

KJ_TEST("test recoverable exception callback") {
  rust::test::TestCallback callback = [](size_t a, size_t b) -> size_t {
    kj::throwRecoverableException(KJ_EXCEPTION(DISCONNECTED, "Premature EOF."));
    KJ_UNREACHABLE;
  };
  // std::terminate is called when c++ throws unhandled recoverable exception
  KJ_EXPECT_SIGNAL(SIGABRT, rust::test::call_callback(callback, 40, 2));
}

KJ_TEST("shared structure") {
  {
    // rust structure arguments are passed by-value in c++
    const auto s = rust::test::SharedStruct{.a = 20, .b = 22};
    KJ_EXPECT(42 == rust::test::pass_shared_struct(s));
  }

  {
    // rust structure return values are return by-value in c++
    const auto s = rust::test::return_shared_struct();
    KJ_EXPECT(13 == s.a);
    KJ_EXPECT(29 == s.b);
  }

  {
    // rust reference looks like const reference to c++;
    const auto s = rust::test::SharedStruct{.a = 20, .b = 22};
    KJ_EXPECT(42 == rust::test::pass_shared_struct_as_ref(s));
  }

  {
    // rust mutable reference looks like reference to c++
    auto s = rust::test::SharedStruct{.a = 10, .b = 32};
    rust::test::pass_shared_struct_as_mut_ref(s);
    KJ_EXPECT(s.a == 42);
    KJ_EXPECT(s.b == 0);
  }

  {
    // rust const pointer looks like const pointer to c++
    const auto s = rust::test::SharedStruct{.a = 20, .b = 22};
    KJ_EXPECT(42 == rust::test::pass_shared_struct_as_const_ptr(&s));
  }

  {
    // rust mut pointer looks like pointer to c++
    auto s = rust::test::SharedStruct{.a = 10, .b = 32};
    rust::test::pass_shared_struct_as_mut_ptr(&s);
    KJ_EXPECT(s.a == 0);
    KJ_EXPECT(s.b == 0);
  }

  {
    // rust Box<T> type is represented as special ::rust::Box<T> c++ type
    // there are many ways to create a Box

    {
      // box can be created by copying the value
      auto box = ::rust::Box<rust::test::SharedStruct>(rust::test::SharedStruct{.a = 3, .b = 39});
      // box is consumed by the call as expected
      KJ_EXPECT(42 == rust::test::pass_shared_struct_as_box(kj::mv(box)));
    }

    {
      // box can be created by moving the value
      auto s = rust::test::SharedStruct{.a = 3, .b = 39};
      auto box = ::rust::Box<rust::test::SharedStruct>(kj::mv(s));
      KJ_EXPECT(42 == rust::test::pass_shared_struct_as_box(kj::mv(box)));
    }

    {
      // box can be created from raw pointer.
      // Memory needs to be allocated using malloc, since rust doesn't have
      // access to delete.
      auto mem = malloc(sizeof(rust::test::SharedStruct));
      auto s = new (mem) rust::test::SharedStruct;
      s->a = 4;
      s->b = 38;
      auto box = ::rust::Box<rust::test::SharedStruct>::from_raw(s);
      KJ_EXPECT(42 == rust::test::pass_shared_struct_as_box(kj::mv(box)));
    }
  }

  {
    // box can be returned from rust to c++ as well
    auto box = rust::test::return_shared_struct_as_box();
    KJ_EXPECT(1 == box->a);
    KJ_EXPECT(41 == box->b);
  }
}

KJ_TEST("opaque rust type") {
  // &str is represented as a special ::rust::Str type
  // it supports variety of implicit constructors.
  auto s = rust::test::rust_struct_new_box("test_name");
  ::rust::Str name = s->get_name();

  // ::rust::Str is _not_ null-terminated so kj::StringPtr can't be created from
  // it. need to allocate to create c++-string (or use it as ArrayPtr).
  auto strName = (std::string)name;
  KJ_EXPECT("test_name"_kj == kj::StringPtr(strName.c_str()));

  s->set_name("another_name");
  KJ_EXPECT("another_name"_kj == kj::StringPtr((std::string)s->get_name()));
}

KJ_TEST("rust::String test") {
  auto s = rust::test::get_string();
  auto expected = "rust_string"_kj;
  KJ_EXPECT(expected == kj::str(s));
  KJ_EXPECT(expected == kj::toCharSequence(s));
  KJ_EXPECT(kj::hashCode(expected) == kj::hashCode(s));
}

KJ_TEST("rust::str test") {
  auto s = rust::test::get_str();
  auto expected = "rust_str"_kj;
  KJ_EXPECT(expected == kj::str(s));
  KJ_EXPECT(expected == kj::toCharSequence(s));
  KJ_EXPECT(kj::hashCode(expected) == kj::hashCode(s));
}

KJ_TEST("test async immediate future") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto pair = kj::newPromiseAndCrossThreadFulfiller<size_t>();

  rust::test::UsizeCallback callback = [&](size_t a) { pair.fulfiller->fulfill(kj::mv(a)); };
  rust::test::async_immediate(callback);

  auto result = pair.promise.wait(waitScope);
  KJ_EXPECT(result == 42);
}

KJ_TEST("test async delay") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto pair = kj::newPromiseAndCrossThreadFulfiller<size_t>();

  rust::test::UsizeCallback callback = [&](size_t a) { pair.fulfiller->fulfill(kj::mv(a)); };
  rust::test::async_sleep(callback);

  auto result = pair.promise.wait(waitScope);
  KJ_EXPECT(result == 42);
}

KJ_TEST("array/slice convertions") {
  // const arrayPtr -> const slice
  {
    kj::ArrayPtr<const kj::byte> a = "foo"_kjb;
    ::rust::Slice<const kj::byte> s = a.as<Rust>();
    KJ_EXPECT(s.length() == a.size());
  }

  // mutable arrayPtr -> const slice
  {
    kj::Array<kj::byte> a = kj::heapArray<kj::byte>(20);
    kj::ArrayPtr<kj::byte> p = a.asPtr();
    ::rust::Slice<const kj::byte> s = p.as<Rust>();
    KJ_EXPECT(s.length() == a.size());
  }

  // mutable arrayPtr -> mutable slice
  {
    kj::Array<kj::byte> a = kj::heapArray<kj::byte>(20);
    ::rust::Slice<kj::byte> s = a.asPtr().as<RustMutable>();
    KJ_EXPECT(s.length() == a.size());
  }

  // const array -> const slice
  {
    kj::Array<const kj::byte> a = kj::heapArray<kj::byte>(20);
    ::rust::Slice<const kj::byte> s = a.as<Rust>();
    KJ_EXPECT(s.length() == a.size());
  }

  // mutable array -> const slice
  {
    kj::Array<kj::byte> a = kj::heapArray<kj::byte>(20);
    ::rust::Slice<const kj::byte> s = a.as<Rust>();
    KJ_EXPECT(s.length() == a.size());
  }

  // mutable array -> mutable slice
  {
    kj::Array<kj::byte> a = kj::heapArray<kj::byte>(20);
    ::rust::Slice<kj::byte> s = a.as<RustMutable>();
    KJ_EXPECT(s.length() == a.size());
  }
}

}  // namespace edgeworker::tests
