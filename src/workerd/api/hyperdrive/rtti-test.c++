// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The C++ Hyperdrive is the one registered with the isolate, so the RTTI export, and the types
// generated from it, describe it while the RUST_HYPERDRIVE autogate makes bindings of the Rust
// one. This test checks that the Rust Hyperdrive exports the same RTTI, so that the types match
// what runs, and so that the Rust one can become the source of the types once the C++ one is
// removed.

#include <workerd/api/hyperdrive.h>
#include <workerd/api/hyperdrive/rtti_test_bridge.rs.h>
#include <workerd/api/sockets.h>
// The RTTI of Socket, which connect() returns, refers to the streams' controllers.
#include <workerd/api/streams.h>
#include <workerd/api/streams/standard.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/jsg/rtti.h>

#include <capnp/message.h>
#include <capnp/serialize-text.h>
#include <capnp/serialize.h>
#include <kj/array.h>
#include <kj/test.h>
#include <kj/vector.h>

#include <algorithm>

namespace workerd::api {
namespace {

// The RTTI of a type as the TypeScript generator reads it. The C++ builder records the C++ type
// behind a string or a number, which the generator ignores; the Rust one records the JS type.
void normalizeType(jsg::rtti::Type::Builder type) {
  switch (type.which()) {
    case jsg::rtti::Type::NUMBER:
      type.getNumber().setName("");
      break;
    case jsg::rtti::Type::STRING:
      type.getString().setName("");
      break;
    case jsg::rtti::Type::MAYBE:
      type.getMaybe().setName("");
      normalizeType(type.getMaybe().getValue());
      break;
    case jsg::rtti::Type::PROMISE:
      normalizeType(type.getPromise().getValue());
      break;
    case jsg::rtti::Type::ARRAY:
      type.getArray().setName("");
      normalizeType(type.getArray().getElement());
      break;
    default:
      break;
  }
}

// `structure` as the TypeScript generator reads it, written to `out`:
//
// - Without its fully qualified name, which names the implementation (`workerd::api::Hyperdrive`
//   or `hyperdrive::Hyperdrive`), nor its tsName: the generator falls back to the name, which
//   the Rust tsName repeats.
// - Without the jsg::Lock argument C++ declares, which is not a JS argument, and without
//   fastApiCompatible and argNames, which the generator does not read.
// - With its members in name order. The Rust macro registers methods before properties and C++
//   in declaration order; neither order is observable in JS, where the properties are on the
//   instance and connect() on the prototype, and the generated types take Hyperdrive's member
//   order from types/defines/hyperdrive.d.ts.
void normalize(jsg::rtti::Structure::Reader structure, jsg::rtti::Structure::Builder out) {
  out.setName(structure.getName());
  out.setTsRoot(structure.getTsRoot());
  out.setIterable(structure.getIterable());
  out.setAsyncIterable(structure.getAsyncIterable());
  out.setDisposable(structure.getDisposable());
  out.setAsyncDisposable(structure.getAsyncDisposable());
  out.setTsOverride(structure.getTsOverride());
  out.setTsDefine(structure.getTsDefine());
  if (structure.hasExtends()) out.setExtends(structure.getExtends());
  if (structure.hasCallable()) out.setCallable(structure.getCallable());
  if (structure.hasBuiltinModules()) out.setBuiltinModules(structure.getBuiltinModules());

  auto memberName = [](jsg::rtti::Member::Reader member) -> kj::StringPtr {
    switch (member.which()) {
      case jsg::rtti::Member::METHOD:
        return member.getMethod().getName();
      case jsg::rtti::Member::PROPERTY:
        return member.getProperty().getName();
      case jsg::rtti::Member::NESTED:
        return member.getNested().getName();
      case jsg::rtti::Member::CONSTANT:
        return member.getConstant().getName();
      case jsg::rtti::Member::CONSTRUCTOR:
        return "constructor"_kj;
    }
    KJ_UNREACHABLE;
  };
  auto members = KJ_MAP(member, structure.getMembers()) { return member; };
  std::sort(members.begin(), members.end(),
      [&](auto a, auto b) { return memberName(a) < memberName(b); });

  auto outMembers = out.initMembers(members.size());
  for (auto i: kj::indices(members)) {
    auto member = members[i];
    auto outMember = outMembers[i];
    switch (member.which()) {
      case jsg::rtti::Member::METHOD: {
        auto method = member.getMethod();
        auto outMethod = outMember.initMethod();
        outMethod.setName(method.getName());
        outMethod.setStatic(method.getStatic());
        outMethod.setReturnType(method.getReturnType());
        normalizeType(outMethod.getReturnType());
        kj::Vector<jsg::rtti::Type::Reader> args;
        for (auto arg: method.getArgs()) {
          if (arg.isJsgImpl() &&
              arg.getJsgImpl().getType() == jsg::rtti::JsgImplType::Type::JSG_LOCK) {
            continue;
          }
          args.add(arg);
        }
        auto outArgs = outMethod.initArgs(args.size());
        for (auto j: kj::indices(args)) {
          outArgs.setWithCaveats(j, args[j]);
          normalizeType(outArgs[j]);
        }
        break;
      }
      case jsg::rtti::Member::PROPERTY: {
        auto property = member.getProperty();
        auto outProperty = outMember.initProperty();
        outProperty.setName(property.getName());
        outProperty.setType(property.getType());
        normalizeType(outProperty.getType());
        outProperty.setReadonly(property.getReadonly());
        outProperty.setLazy(property.getLazy());
        outProperty.setPrototype(property.getPrototype());
        break;
      }
      case jsg::rtti::Member::NESTED:
      case jsg::rtti::Member::CONSTANT:
      case jsg::rtti::Member::CONSTRUCTOR:
        KJ_FAIL_ASSERT("Hyperdrive has no member of this kind", memberName(member));
    }
  }
}

kj::String describe(jsg::rtti::Structure::Reader structure) {
  capnp::MallocMessageBuilder message;
  auto out = message.initRoot<jsg::rtti::Structure>();
  normalize(structure, out);
  capnp::TextCodec codec;
  codec.setPrettyPrint(true);
  return codec.encode(out.asReader());
}

KJ_TEST("the Rust Hyperdrive exports the same RTTI as the C++ one") {
  jsg::rtti::Builder<CompatibilityFlags::Reader> builder((CompatibilityFlags::Reader()));
  auto expected = describe(builder.structure<Hyperdrive>());

  auto bytes = hyperdrive_test::hyperdrive_rtti();
  auto words = kj::heapArray<capnp::word>(bytes.size() / sizeof(capnp::word));
  memcpy(words.begin(), bytes.data(), words.asBytes().size());
  capnp::FlatArrayMessageReader reader(words);
  auto groups = reader.getRoot<jsg::rtti::StructureGroups>().getGroups();
  KJ_ASSERT(groups.size() == 1);
  // The Rust Hyperdrive refers to no other Rust structure: Socket is the C++ one.
  auto structures = groups[0].getStructures();
  KJ_ASSERT(structures.size() == 1);
  auto actual = describe(structures[0]);

  KJ_EXPECT(actual == expected, actual, expected);
  // The members are compared, not two empty lists.
  KJ_EXPECT(structures[0].getMembers().size() == 8);
}

}  // namespace
}  // namespace workerd::api
