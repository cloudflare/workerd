// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/jsg/unwrap-args.h>

#include <kj/string.h>
#include <kj/test.h>
#include <kj/tuple.h>
#include <kj/vector.h>

namespace workerd::jsg::test {
namespace {

// A probe whose constructor records the value of a global counter, then
// increments it.  When N probes are constructed in sequence, their `order`
// fields equal 0, 1, 2, ..., N-1 in the order they were constructed.
struct OrderProbe {
  int order;
  OrderProbe(): order(counter++) {}
  static inline int counter = 0;
};

KJ_TEST("UnwrappedArgs constructs elements left-to-right") {
  using Indexes = kj::_::Indexes<0, 1, 2, 3>;

  OrderProbe::counter = 0;
  auto makeProbe = []<size_t I, typename U>() -> OrderProbe { return OrderProbe{}; };

  jsg::_::UnwrappedArgs<Indexes, OrderProbe, OrderProbe, OrderProbe, OrderProbe> args(makeProbe);

  // After construction, each slot's `order` field records the sequential
  // value the counter had when that slot's probe was constructed.
  // Left-to-right construction means slot 0 was first.  Locals avoid
  // unparenthesised `<` inside the KJ_EXPECT macro expansion.
  int order0 = static_cast<jsg::_::UnwrappedArg<0, OrderProbe>&>(args).value.order;
  int order1 = static_cast<jsg::_::UnwrappedArg<1, OrderProbe>&>(args).value.order;
  int order2 = static_cast<jsg::_::UnwrappedArg<2, OrderProbe>&>(args).value.order;
  int order3 = static_cast<jsg::_::UnwrappedArg<3, OrderProbe>&>(args).value.order;
  KJ_EXPECT(order0 == 0);
  KJ_EXPECT(order1 == 1);
  KJ_EXPECT(order2 == 2);
  KJ_EXPECT(order3 == 3);
}

KJ_TEST("UnwrappedArgs invokes callable with correct compile-time Index and Type") {
  using Indexes = kj::_::Indexes<0, 1, 2>;

  // Record each (index, type) the unwrap callable is invoked with.
  struct Call {
    size_t index;
    kj::StringPtr typeName;
  };
  kj::Vector<Call> calls;

  auto recordingUnwrap = [&calls]<size_t I, typename U>() -> int {
    if constexpr (kj::isSameType<U, int>()) {
      calls.add(Call{I, "int"_kj});
    } else if constexpr (kj::isSameType<U, double>()) {
      calls.add(Call{I, "double"_kj});
    } else if constexpr (kj::isSameType<U, bool>()) {
      calls.add(Call{I, "bool"_kj});
    } else {
      calls.add(Call{I, "unknown"_kj});
    }
    return static_cast<int>(I);
  };

  jsg::_::UnwrappedArgs<Indexes, int, double, bool> args(recordingUnwrap);

  KJ_ASSERT(calls.size() == 3);
  KJ_EXPECT(calls[0].index == 0);
  KJ_EXPECT(calls[0].typeName == "int");
  KJ_EXPECT(calls[1].index == 1);
  KJ_EXPECT(calls[1].typeName == "double");
  KJ_EXPECT(calls[2].index == 2);
  KJ_EXPECT(calls[2].typeName == "bool");
}

KJ_TEST("UnwrappedArgs take<I>() moves values out of the I'th slot") {
  using Indexes = kj::_::Indexes<0, 1, 2>;

  auto unwrap = []<size_t I, typename U>() -> kj::String { return kj::str("slot-", I); };

  jsg::_::UnwrappedArgs<Indexes, kj::String, kj::String, kj::String> args(unwrap);

  kj::String a = kj::mv(args).template take<0>();
  kj::String b = kj::mv(args).template take<1>();
  kj::String c = kj::mv(args).template take<2>();

  KJ_EXPECT(a == "slot-0");
  KJ_EXPECT(b == "slot-1");
  KJ_EXPECT(c == "slot-2");
}

KJ_TEST("UnwrappedArgs take<I>() forwards rvalue-ref parameter types as rvalue refs") {
  // Rvalue-ref parameters (e.g. `JsgStruct&&` for move-in, as
  // `HTMLRewriter::on` does with `ElementContentHandlers&&`) need to come
  // out as rvalue references that bind to T&& method parameters.  Because
  // `RemoveRvalueRef` strips the `&&`, the stored value is held by value
  // (owned by the helper) rather than as a dangling rvalue-ref member.
  // `take<I>()` then forwards it back as an rvalue ref via reference
  // collapsing on `kj::fwd<T&&>`.
  using Indexes = kj::_::Indexes<0>;

  auto unwrap = []<size_t I, typename U>() -> kj::String { return kj::str("moved"); };

  jsg::_::UnwrappedArgs<Indexes, kj::String&&> args(unwrap);

  // take<0>() must return `kj::String&&` so it binds to a T&& method
  // parameter, allowing move-in semantics at the call site.
  static_assert(kj::isSameType<decltype(kj::mv(args).template take<0>()), kj::String&&>(),
      "take<I>() of T&& parameter must return T&&, enabling move into the call site");

  kj::String s = kj::mv(args).template take<0>();
  KJ_EXPECT(s == "moved");
}

KJ_TEST("UnwrappedArgs take<I>() preserves reference parameter types as lvalue refs") {
  // For reference-typed parameters (e.g. `Lock&`, `TypeHandler<T>&`), the
  // stored value is a reference member.  `take<I>()` must return an lvalue
  // reference, not an rvalue reference — otherwise the value would not bind
  // to a non-const lvalue-ref parameter at the JSG call site.
  using Indexes = kj::_::Indexes<0, 1>;

  int x = 10;
  int y = 20;
  auto unwrap = [&]<size_t I, typename U>() -> int& { return I == 0 ? x : y; };

  jsg::_::UnwrappedArgs<Indexes, int&, int&> args(unwrap);

  // take<0>() must return `int&` so we can mutate through it.
  static_assert(kj::isSameType<decltype(kj::mv(args).template take<0>()), int&>(),
      "take<I>() of int& parameter must return int&, not int&&");

  int& a = kj::mv(args).template take<0>();
  int& b = kj::mv(args).template take<1>();
  a = 100;
  b = 200;
  KJ_EXPECT(x == 100);
  KJ_EXPECT(y == 200);
}

// Records its destruction in a shared vector.  Used to verify that when
// construction of one slot throws, earlier slots are destroyed in reverse
// order — standard C++ subobject unwinding behavior.
struct DestructionProbe {
  kj::Vector<int>& destructions;
  int order;
  DestructionProbe(kj::Vector<int>& d, int o): destructions(d), order(o) {}
  ~DestructionProbe() {
    destructions.add(order);
  }
  // Disallow copy and move so each slot's destructor fires exactly once.
  // Returning a DestructionProbe by value is still legal thanks to
  // guaranteed copy elision for prvalues (C++17+).
  KJ_DISALLOW_COPY_AND_MOVE(DestructionProbe);
};

KJ_TEST("UnwrappedArgs unwinds partially-constructed bases in reverse on throw") {
  using Indexes = kj::_::Indexes<0, 1, 2>;

  kj::Vector<int> unwound;

  auto unwrapOrThrow = [&]<size_t I, typename U>() -> DestructionProbe {
    if constexpr (I == 2) {
      KJ_FAIL_REQUIRE("construction failure at slot 2");
    } else {
      return DestructionProbe{unwound, static_cast<int>(I)};
    }
  };

  KJ_EXPECT_THROW_MESSAGE("construction failure at slot 2",
      (jsg::_::UnwrappedArgs<Indexes, DestructionProbe, DestructionProbe, DestructionProbe>(
          unwrapOrThrow)));

  // Slot 0 and slot 1 were constructed; slot 2 threw before its body
  // produced a probe.  C++ destroys already-constructed base subobjects
  // in reverse declaration order, so slot 1 is destroyed first, then
  // slot 0.
  KJ_ASSERT(unwound.size() == 2);
  KJ_EXPECT(unwound[0] == 1);
  KJ_EXPECT(unwound[1] == 0);
}

}  // namespace
}  // namespace workerd::jsg::test
