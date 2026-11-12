// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// C++ reads test.capnp's types through the cxx bridge that capnpc-rust-serde generates for
// wd_rust_serde_capnp_library(ffi = True). capnp-serde-test checks the JSON itself against the C++
// codec; this checks what the getters return.

#include <workerd/rust/capnp-serde/tests/test_serde_ffi.rs.h>

#include <kj-rs/convert.h>

#include <kj/string.h>
#include <kj/test.h>

#include <cmath>
#include <cstdint>

namespace capnp_serde::test {
namespace {

using kj_rs::Rust;

rust::Slice<const uint8_t> bytes(kj::StringPtr json) {
  return json.asBytes().as<Rust>();
}

kj::ArrayPtr<const kj::byte> view(rust::Slice<const uint8_t> slice) {
  return kj::from<Rust>(slice);
}

KJ_TEST("primitives") {
  auto value = parsePrimitives(bytes(R"({"bool": true, "int8": -8, "int16": -16, "int32": -32,
      "int64": "-64", "uint8": 8, "uint16": 16, "uint32": 32, "uint64": "18446744073709551615",
      "float32": 1.5, "float64": "-Infinity", "color": "blue"})"));
  KJ_EXPECT(value->getBool());
  KJ_EXPECT(value->getInt8() == -8);
  KJ_EXPECT(value->getInt16() == -16);
  KJ_EXPECT(value->getInt32() == -32);
  KJ_EXPECT(value->getInt64() == -64);
  KJ_EXPECT(value->getUint8() == 8);
  KJ_EXPECT(value->getUint16() == 16);
  KJ_EXPECT(value->getUint32() == 32);
  KJ_EXPECT(value->getUint64() == UINT64_MAX);
  KJ_EXPECT(value->getFloat32() == 1.5f);
  KJ_EXPECT(value->getFloat64() == -INFINITY);
  KJ_EXPECT(value->getColor() == Color::DarkBlue);

  auto empty = parsePrimitives(bytes("{}"));
  KJ_EXPECT(!empty->getBool());
  KJ_EXPECT(empty->getUint64() == 0);
  KJ_EXPECT(empty->getColor() == Color::Red);
}

KJ_TEST("missing fields have their schema defaults") {
  auto value = parseDefaults(bytes("{}"));
  KJ_EXPECT(value->getBool());
  KJ_EXPECT(value->getInt8() == -8);
  KJ_EXPECT(value->getInt64() == -64);
  KJ_EXPECT(value->getUint64() == UINT64_MAX);
  KJ_EXPECT(value->getFloat32() == 1.5f);
  KJ_EXPECT(value->getFloat64() == INFINITY);
  KJ_EXPECT(std::isnan(value->getNan()));
  KJ_EXPECT(value->getColor() == Color::LightGreen);
}

KJ_TEST("pointers are read in place") {
  kj::StringPtr json = R"({"text": "text", "data": [97, 98, 99], "base64": "YWJj",
      "hex": "616263", "ints": ["-1", "2"], "bytes": [0, 255], "texts": ["x", "y"],
      "structs": [{"int8": 1}, {}], "primitives": {"uint32": 5},
      "not-a-rust-identifier": "renamed", "self": "self", "match": "match"})";
  auto value = parsePointers(bytes(json));
  KJ_EXPECT(value->hasText());
  KJ_EXPECT(kj::str(value->getText()) == "text");
  KJ_EXPECT(view(value->getData()) == "abc"_kjb);
  KJ_EXPECT(view(value->getBase64()) == "abc"_kjb);
  KJ_EXPECT(view(value->getHex()) == "abc"_kjb);
  auto ints = value->getInts();
  KJ_ASSERT(ints.size() == 2);
  KJ_EXPECT(ints[0] == -1);
  KJ_EXPECT(ints[1] == 2);
  KJ_EXPECT(view(value->getBytes()) == kj::arr<kj::byte>(0, 255));
  auto texts = KJ_MAP(text, value->getTexts()) { return kj::str(text); };
  KJ_EXPECT(kj::strArray(texts, ",") == "x,y");
  auto int8s = KJ_MAP(element, value->getStructs()) { return element.getInt8(); };
  KJ_EXPECT(int8s == kj::arr<int8_t>(1, 0));
  KJ_EXPECT(value->hasPrimitives());
  KJ_EXPECT(value->getPrimitives().getUint32() == 5);
  KJ_EXPECT(kj::str(value->getRenamed()) == "renamed");
  KJ_EXPECT(kj::str(value->getSelf()) == "self");
  KJ_EXPECT(kj::str(value->getMatch()) == "match");

  // The getters borrow from the value.
  KJ_EXPECT(value->getText().data() == value->getText().data());
  KJ_EXPECT(&value->getPrimitives() == &value->getPrimitives());
}

KJ_TEST("null pointers read as empty values") {
  auto value = parsePointers(bytes(R"({"text": null, "hex": null})"));
  KJ_EXPECT(!value->hasText());
  KJ_EXPECT(value->getText().size() == 0);
  KJ_EXPECT(!value->hasHex());
  KJ_EXPECT(value->getHex().size() == 0);
  KJ_EXPECT(!value->hasTexts());
  KJ_EXPECT(value->getTexts().size() == 0);
  KJ_EXPECT(!value->hasStructs());
  KJ_EXPECT(value->getStructs().size() == 0);
  // A null struct reads as the struct's defaults, like a capnp reader's.
  KJ_EXPECT(!value->hasPrimitives());
  KJ_EXPECT(value->getPrimitives().getUint32() == 0);
  KJ_EXPECT(!value->getPrimitives().getBool());
}

KJ_TEST("unions") {
  auto value = parseUnions(bytes(R"({"common": "c", "plain": {"b": null},
      "tagged": {"kind": "x", "x": 3}, "flat": "p", "value": "pee",
      "grouped": {"inGroup": "in", "g2": null}, "flatMember": 9, "number": "5"})"));
  KJ_EXPECT(kj::str(value->getCommon()) == "c");
  KJ_EXPECT(value->which() == UnionsWhich::Number);
  KJ_EXPECT(value->isNumber());
  KJ_EXPECT(!value->isText());
  KJ_EXPECT(value->getNumber() == 5);
  // Members that are not set read as their defaults.
  KJ_EXPECT(value->getText().size() == 0);
  KJ_EXPECT(value->getData().size() == 0);

  auto& plain = value->getPlain();
  KJ_EXPECT(plain.which() == UnionsPlainWhich::B);
  KJ_EXPECT(plain.isB());
  KJ_EXPECT(plain.getA() == 0);

  auto& tagged = value->getTagged();
  KJ_EXPECT(tagged.which() == UnionsTaggedWhich::X);
  KJ_EXPECT(tagged.getX() == 3);
  KJ_EXPECT(tagged.getZ().getInt16() == 0);

  auto& flat = value->getFlat();
  KJ_EXPECT(flat.which() == UnionsFlatWhich::P);
  KJ_EXPECT(kj::str(flat.getP()) == "pee");
  KJ_EXPECT(flat.getQ() == 0);

  auto& grouped = value->getGrouped();
  KJ_EXPECT(kj::str(grouped.getInGroup()) == "in");
  KJ_EXPECT(grouped.which() == UnionsGroupedWhich::G2);
  KJ_EXPECT(grouped.isG2());
  KJ_EXPECT(!grouped.isG1());

  KJ_EXPECT(value->getFlatGroup().getFlatMember() == 9);

  auto other = parseUnions(bytes(R"({"plain": {"a": -1}, "tagged": {"kind": "z", "int16": 7},
      "flat": "renamed-q", "value": "-Infinity", "grouped": {"g1": "one"}, "data": "6162"})"));
  KJ_EXPECT(other->which() == UnionsWhich::Data);
  KJ_EXPECT(view(other->getData()) == "ab"_kjb);
  KJ_EXPECT(other->getNumber() == 0);
  KJ_EXPECT(other->getPlain().getA() == -1);
  KJ_EXPECT(other->getTagged().which() == UnionsTaggedWhich::Z);
  KJ_EXPECT(other->getTagged().getZ().getInt16() == 7);
  KJ_EXPECT(other->getFlat().getQ() == -INFINITY);
  KJ_EXPECT(kj::str(other->getGrouped().getG1()) == "one");
}

KJ_TEST("internally tagged unions") {
  auto foo = parseDiscriminated(bytes(R"({"type": "foo", "foo": 4})"));
  KJ_EXPECT(foo->which() == DiscriminatedWhich::Foo);
  KJ_EXPECT(foo->getFoo() == 4);
  KJ_EXPECT(!foo->getBar().hasText());

  auto bar =
      parseDiscriminated(bytes(R"({"type": "bar", "text": "t", "primitives": {"bool": true}})"));
  KJ_EXPECT(bar->isBar());
  KJ_EXPECT(kj::str(bar->getBar().getText()) == "t");
  KJ_EXPECT(bar->getBar().getPrimitives().getBool());
  KJ_EXPECT(bar->getFoo() == 0);

  auto baz = parseDiscriminated(bytes(R"({"type": "baz", "bazMember": "b"})"));
  KJ_EXPECT(baz->which() == DiscriminatedWhich::Baz);
  KJ_EXPECT(kj::str(baz->getBaz().getBazMember()) == "b");

  auto qux = parseDiscriminated(bytes(R"({"type": "qux"})"));
  KJ_EXPECT(qux->isQux());
  KJ_EXPECT(!qux->getBaz().hasBazMember());
}

KJ_TEST("recursive and boxed values") {
  auto value = parseRecursive(bytes(R"({"name": "root",
      "child": {"name": "child", "cycle": {"other": {"name": "other", "cycle": {"none": null}}}},
      "children": [{"name": "a", "cycle": {"none": null}}, {"name": "b", "cycle": {"none": null}}],
      "cycle": {"none": null}})"));
  KJ_EXPECT(kj::str(value->getName()) == "root");
  KJ_EXPECT(value->getCycle().isNone());
  auto& child = value->getChild();
  KJ_EXPECT(kj::str(child.getName()) == "child");
  KJ_EXPECT(child.getCycle().which() == RecursiveCycleWhich::Other);
  KJ_EXPECT(kj::str(child.getCycle().getOther().getName()) == "other");
  KJ_EXPECT(!child.hasChild());
  KJ_EXPECT(!child.getChild().hasName());
  auto names = KJ_MAP(element, value->getChildren()) { return kj::str(element.getName()); };
  KJ_EXPECT(kj::strArray(names, ",") == "a,b");

  auto boxed = parseBoxed(
      bytes(R"({"name": "n", "outer": {"kind": "none"}, "kind": "member", "label": "l"})"));
  KJ_EXPECT(boxed->hasInner());
  KJ_EXPECT(kj::str(boxed->getInner().getName()) == "n");
  KJ_EXPECT(boxed->getInner().getOuter().isNone());
  KJ_EXPECT(boxed->which() == BoxedWhich::Member);
  KJ_EXPECT(kj::str(boxed->getMember().getLabel()) == "l");
  KJ_EXPECT(!boxed->getMember().hasParent());
}

KJ_TEST("nested declarations and flattened fields") {
  auto outer = parseOuter(bytes(R"({"inner": {"x": 1}, "mode": "on"})"));
  KJ_EXPECT(outer->getInner().getX() == 1);
  KJ_EXPECT(outer->getMode() == OuterMode::On);
  auto empty = parseOuter(bytes("{}"));
  KJ_EXPECT(!empty->hasInner());
  KJ_EXPECT(empty->getInner().getX() == 0);
  KJ_EXPECT(empty->getMode() == OuterMode::Off);

  auto inner = parseOuterInner(bytes(R"({"x": 2})"));
  KJ_EXPECT(inner->getX() == 2);

  auto flattened =
      parseFlattened(bytes(R"({"before": "b", "int32": 5, "color": "blue", "after": "a"})"));
  KJ_EXPECT(kj::str(flattened->getBefore()) == "b");
  KJ_EXPECT(flattened->getInner().getInt32() == 5);
  KJ_EXPECT(flattened->getInner().getColor() == Color::DarkBlue);
  KJ_EXPECT(kj::str(flattened->getAfter()) == "a");
}

KJ_TEST("the input ends at a NUL, as in JsonCodec") {
  // A kj::String's array ends with its NUL terminator.
  auto json = kj::str(R"({"int8": 3})").releaseArray();
  KJ_ASSERT(json.back() == '\0');
  auto value = parsePrimitives(json.asBytes().as<Rust>());
  KJ_EXPECT(value->getInt8() == 3);
  static constexpr char withGarbage[] = "{\"int8\": 4}\0garbage";
  auto garbage = parsePrimitives(kj::asBytes(withGarbage, sizeof(withGarbage) - 1).as<Rust>());
  KJ_EXPECT(garbage->getInt8() == 4);
}

KJ_TEST("invalid JSON throws") {
  KJ_EXPECT_THROW_MESSAGE("invalid type", parsePrimitives(bytes(R"({"int8": []})")));
  KJ_EXPECT_THROW_MESSAGE("EOF", parsePrimitives(bytes("{")));
}

}  // namespace
}  // namespace capnp_serde::test
