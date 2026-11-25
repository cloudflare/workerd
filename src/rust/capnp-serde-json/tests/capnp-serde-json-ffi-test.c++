// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// C++ reads test.capnp's types through the cxx bridge that capnpc-rust-serde-json generates for
// wd_rust_capnp_serde_json_library(ffi = True), and checks them against the C++ capnp::JsonCodec
// with handleByAnnotation(): every document that parse() decodes through the bridge, JsonCodec
// must decode to a message whose fields read the same. Some documents are also checked to be what
// JsonCodec writes with HasMode::NON_DEFAULT, which is what the Rust types write.
// capnp-serde-json-test has the same documents on the Rust side.

#include <workerd/rust/capnp-serde-json/tests/test_serde_json_ffi.rs.h>

#include <kj-rs/convert.h>
#include <rust/capnp-serde-json/tests/test.capnp.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/string.h>
#include <kj/test.h>

#include <cstdint>

namespace capnp_serde_json::test {
namespace {

using kj_rs::Rust;

rust::Slice<const uint8_t> bytes(kj::StringPtr json) {
  return json.asBytes().as<Rust>();
}

kj::ArrayPtr<const kj::byte> view(rust::Slice<const uint8_t> slice) {
  return kj::from<Rust>(slice);
}

// Comparison of what JsonCodec decodes (capnp's readers, the first argument) with what the
// bridge decodes (the second).

// The document being compared, for failure messages.
kj::StringPtr currentJson;

#define EXPECT_SAME(a, b) KJ_EXPECT(same(a, b), #a, currentJson)

template <typename T>
  requires std::is_integral_v<T>
bool same(T a, T b) {
  return a == b;
}

bool same(capnp::Text::Reader a, rust::Str b) {
  return a.asArray() == kj::from<Rust>(b);
}

bool same(capnp::Text::Reader a, const rust::String& b) {
  return same(a, rust::Str(b));
}

bool same(capnp::Data::Reader a, rust::Slice<const uint8_t> b) {
  return a == view(b);
}

template <typename E>
uint16_t ordinal(E value) {
  return static_cast<uint16_t>(value);
}

template <typename CppList, typename RustSlice, typename Compare>
void expectSameList(CppList a, RustSlice b, Compare&& compare) {
  KJ_EXPECT(a.size() == b.size(), a.size(), b.size(), currentJson);
  for (auto i: kj::zeroTo(kj::min(a.size(), b.size()))) {
    compare(a[i], b[i]);
  }
}

template <typename CppList, typename RustSlice>
void expectSameList(CppList a, RustSlice b) {
  expectSameList(a, b, [](auto x, const auto& y) { EXPECT_SAME(x, y); });
}

void expectSame(::Primitives::Reader a, const Primitives& b) {
  EXPECT_SAME(a.getBool(), b.getBool());
  EXPECT_SAME(a.getInt8(), b.getInt8());
  EXPECT_SAME(a.getInt16(), b.getInt16());
  EXPECT_SAME(a.getInt32(), b.getInt32());
  EXPECT_SAME(a.getInt64(), b.getInt64());
  EXPECT_SAME(a.getUint8(), b.getUint8());
  EXPECT_SAME(a.getUint16(), b.getUint16());
  EXPECT_SAME(a.getUint32(), b.getUint32());
  EXPECT_SAME(a.getUint64(), b.getUint64());
}

void expectSame(::Defaults::Reader a, const Defaults& b) {
  EXPECT_SAME(a.getBool(), b.getBool());
  EXPECT_SAME(a.getInt8(), b.getInt8());
  EXPECT_SAME(a.getInt64(), b.getInt64());
  EXPECT_SAME(a.getUint32(), b.getUint32());
  EXPECT_SAME(a.getUint64(), b.getUint64());
}

void expectSame(::Pointers::Reader a, const Pointers& b) {
  EXPECT_SAME(a.hasText(), b.hasText());
  EXPECT_SAME(a.getText(), b.getText());
  EXPECT_SAME(a.hasBase64(), b.hasBase64());
  EXPECT_SAME(a.getBase64(), b.getBase64());
  EXPECT_SAME(a.hasHex(), b.hasHex());
  EXPECT_SAME(a.getHex(), b.getHex());
  EXPECT_SAME(a.hasInts(), b.hasInts());
  expectSameList(a.getInts(), b.getInts());
  EXPECT_SAME(a.hasShorts(), b.hasShorts());
  expectSameList(a.getShorts(), b.getShorts());
  EXPECT_SAME(a.hasTexts(), b.hasTexts());
  expectSameList(a.getTexts(), b.getTexts());
  EXPECT_SAME(a.hasStructs(), b.hasStructs());
  expectSameList(a.getStructs(), b.getStructs(),
      [](::Primitives::Reader x, const Primitives& y) { expectSame(x, y); });
  EXPECT_SAME(a.hasPrimitives(), b.hasPrimitives());
  expectSame(a.getPrimitives(), b.getPrimitives());
  EXPECT_SAME(a.hasRenamed(), b.hasRenamed());
  EXPECT_SAME(a.getRenamed(), b.getRenamed());
  EXPECT_SAME(a.hasSelf(), b.hasSelf());
  EXPECT_SAME(a.getSelf(), b.getSelf());
  EXPECT_SAME(a.hasMatch(), b.hasMatch());
  EXPECT_SAME(a.getMatch(), b.getMatch());
}

// capnp's getter of a union member that is not set fails, so only the member that is set is
// compared.
void expectSame(::Unions::Reader a, const Unions& b) {
  EXPECT_SAME(a.hasCommon(), b.hasCommon());
  EXPECT_SAME(a.getCommon(), b.getCommon());

  EXPECT_SAME(ordinal(a.which()), ordinal(b.which()));
  switch (a.which()) {
    case ::Unions::NUMBER:
      EXPECT_SAME(a.getNumber(), b.getNumber());
      break;
    case ::Unions::TEXT:
      EXPECT_SAME(a.getText(), b.getText());
      break;
    case ::Unions::DATA:
      EXPECT_SAME(a.getData(), b.getData());
      break;
    case ::Unions::PRIMITIVES:
      expectSame(a.getPrimitives(), b.getPrimitives());
      break;
  }

  auto named = a.getNamed();
  EXPECT_SAME(ordinal(named.which()), ordinal(b.getNamed().which()));
  switch (named.which()) {
    case ::Unions::Named::A:
      EXPECT_SAME(named.getA(), b.getNamed().getA());
      break;
    case ::Unions::Named::B:
      expectSameList(named.getB(), b.getNamed().getB());
      break;
  }
}

void expectSame(::Etag::Reader a, const Etag& b) {
  EXPECT_SAME(a.hasValue(), b.hasValue());
  EXPECT_SAME(a.getValue(), b.getValue());
  EXPECT_SAME(ordinal(a.getKind().which()), ordinal(b.getKind().which()));
}

// A member of a pointer type that a tag selects without a value is null in C++, but not in Rust
// (see lib.rs), so hasX() of the members is not compared.
void expectSame(::Tagged::Reader a, const Tagged& b) {
  EXPECT_SAME(a.getId(), b.getId());
  auto payload = a.getPayload();
  EXPECT_SAME(ordinal(payload.which()), ordinal(b.getPayload().which()));
  switch (payload.which()) {
    case ::Tagged::Payload::FOO:
      break;
    case ::Tagged::Payload::BAR:
      expectSame(payload.getBar(), b.getPayload().getBar());
      break;
    case ::Tagged::Payload::ETAG:
      expectSame(payload.getEtag(), b.getPayload().getEtag());
      break;
  }
}

void expectSame(::Etags::Reader a, const Etags& b) {
  EXPECT_SAME(a.hasEtags(), b.hasEtags());
  expectSameList(
      a.getEtags(), b.getEtags(), [](::Etag::Reader x, const Etag& y) { expectSame(x, y); });
}

void expectSame(::Recursive::Reader a, const Recursive& b) {
  EXPECT_SAME(a.hasName(), b.hasName());
  EXPECT_SAME(a.getName(), b.getName());
  EXPECT_SAME(a.hasChildren(), b.hasChildren());
  expectSameList(a.getChildren(), b.getChildren(),
      [](::Recursive::Reader x, const Recursive& y) { expectSame(x, y); });
}

void expectSame(::Outer::Inner::Reader a, const OuterInner& b) {
  EXPECT_SAME(a.getX(), b.getX());
}

void expectSame(::Outer::Reader a, const Outer& b) {
  EXPECT_SAME(a.hasInner(), b.hasInner());
  expectSame(a.getInner(), b.getInner());
  EXPECT_SAME(a.getCount(), b.getCount());
}

template <typename T>
kj::Own<capnp::MallocMessageBuilder> decode(kj::StringPtr json) {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<T>();
  auto message = kj::heap<capnp::MallocMessageBuilder>();
  codec.decode(json, message->initRoot<T>());
  return message;
}

// Decodes `json` as a `T` through the bridge, checks that JsonCodec decodes it to the same
// message, and returns the bridge's value.
template <typename T, typename Value>
rust::Box<Value> parse(
    kj::StringPtr json, rust::Box<Value> (*parseValue)(rust::Slice<const uint8_t>)) {
  currentJson = json;
  auto value = parseValue(bytes(json));
  auto message = decode<T>(json);
  expectSame(message->template getRoot<T>().asReader(), *value);
  return value;
}

// Checks that JsonCodec with HasMode::NON_DEFAULT writes the message it decodes from `json` as
// `json`.
template <typename T>
void expectEncodes(kj::StringPtr json) {
  auto message = decode<T>(json);
  capnp::JsonCodec codec;
  codec.handleByAnnotation<T>();
  codec.setHasMode(capnp::HasMode::NON_DEFAULT);
  auto encoded = codec.encode(message->template getRoot<T>().asReader());
  KJ_EXPECT(encoded == json, encoded, json);
}

template <typename Value>
bool rejects(kj::StringPtr json, rust::Box<Value> (*parseValue)(rust::Slice<const uint8_t>)) {
  return kj::runCatchingExceptions([&]() { parseValue(bytes(json)); }) != kj::none;
}

KJ_TEST("primitives") {
  auto value = parse<::Primitives>(R"({"bool": true, "int8": -8, "int16": -16, "int32": -32,
      "int64": "-64", "uint8": 8, "uint16": 16, "uint32": 32, "uint64": "18446744073709551615"})",
      parsePrimitives);
  KJ_EXPECT(value->getBool());
  KJ_EXPECT(value->getInt8() == -8);
  KJ_EXPECT(value->getInt16() == -16);
  KJ_EXPECT(value->getInt32() == -32);
  KJ_EXPECT(value->getInt64() == -64);
  KJ_EXPECT(value->getUint8() == 8);
  KJ_EXPECT(value->getUint16() == 16);
  KJ_EXPECT(value->getUint32() == 32);
  KJ_EXPECT(value->getUint64() == UINT64_MAX);

  auto empty = parse<::Primitives>("{}", parsePrimitives);
  KJ_EXPECT(!empty->getBool());
  KJ_EXPECT(empty->getUint64() == 0);
}

KJ_TEST("missing fields have their schema defaults") {
  auto value = parse<::Defaults>("{}", parseDefaults);
  KJ_EXPECT(value->getBool());
  KJ_EXPECT(value->getInt8() == -8);
  KJ_EXPECT(value->getInt64() == -64);
  KJ_EXPECT(value->getUint32() == UINT32_MAX);
  KJ_EXPECT(value->getUint64() == UINT64_MAX);
}

KJ_TEST("pointers are read in place") {
  auto value = parse<::Pointers>(R"({"text": "text", "base64": "YWJj", "hex": "616263",
      "ints": ["-1", "2"], "shorts": [0, 65535], "texts": ["x", "y"],
      "structs": [{"int8": 1}, {}], "primitives": {"uint32": 5},
      "not-a-rust-identifier": "renamed", "self": "self", "match": "match"})",
      parsePointers);
  KJ_EXPECT(value->hasText());
  KJ_EXPECT(kj::str(value->getText()) == "text");
  KJ_EXPECT(view(value->getBase64()) == "abc"_kjb);
  KJ_EXPECT(view(value->getHex()) == "abc"_kjb);
  auto ints = value->getInts();
  KJ_ASSERT(ints.size() == 2);
  KJ_EXPECT(ints[0] == -1);
  KJ_EXPECT(ints[1] == 2);
  auto shorts = value->getShorts();
  KJ_ASSERT(shorts.size() == 2);
  KJ_EXPECT(shorts[1] == 65535);
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
  auto value =
      parse<::Pointers>(R"({"text": null, "ints": null, "primitives": null})", parsePointers);
  KJ_EXPECT(!value->hasText());
  KJ_EXPECT(value->getText().size() == 0);
  KJ_EXPECT(!value->hasInts());
  KJ_EXPECT(value->getInts().size() == 0);
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

KJ_TEST("externally tagged unions") {
  auto value = parse<::Unions>(R"({"common": "c", "a": 3, "number": "5"})", parseUnions);
  KJ_EXPECT(kj::str(value->getCommon()) == "c");
  KJ_EXPECT(value->which() == UnionsWhich::Number);
  KJ_EXPECT(value->isNumber());
  KJ_EXPECT(!value->isText());
  KJ_EXPECT(value->getNumber() == 5);
  // Members that are not set read as their defaults.
  KJ_EXPECT(value->getText().size() == 0);
  KJ_EXPECT(value->getData().size() == 0);
  KJ_EXPECT(value->getPrimitives().getInt16() == 0);

  auto& named = value->getNamed();
  KJ_EXPECT(named.which() == UnionsNamedWhich::A);
  KJ_EXPECT(named.isA());
  KJ_EXPECT(named.getA() == 3);
  KJ_EXPECT(named.getB().size() == 0);

  auto other = parse<::Unions>(R"({"b": ["x"], "data": "6162"})", parseUnions);
  KJ_EXPECT(other->which() == UnionsWhich::Data);
  KJ_EXPECT(view(other->getData()) == "ab"_kjb);
  KJ_EXPECT(other->getNumber() == 0);
  KJ_EXPECT(other->getNamed().isB());
  KJ_EXPECT(other->getNamed().getB().size() == 1);

  auto primitives = parse<::Unions>(R"({"a": 0, "primitives": {"int16": 7}})", parseUnions);
  KJ_EXPECT(primitives->isPrimitives());
  KJ_EXPECT(primitives->getPrimitives().getInt16() == 7);

  auto text = parse<::Unions>(R"({"a": 0, "string": "t"})", parseUnions);
  KJ_EXPECT(text->isText());
  KJ_EXPECT(kj::str(text->getText()) == "t");
}

KJ_TEST("internally tagged unions") {
  auto foo = parse<::Tagged>(R"({"id": 1, "type": "foo"})", parseTagged);
  KJ_EXPECT(foo->getId() == 1);
  auto& fooPayload = foo->getPayload();
  KJ_EXPECT(fooPayload.which() == TaggedPayloadWhich::Foo);
  KJ_EXPECT(fooPayload.isFoo());
  KJ_EXPECT(!fooPayload.getBar().hasText());

  auto bar =
      parse<::Tagged>(R"({"type": "bar", "text": "t", "primitives": {"bool": true}})", parseTagged);
  auto& barPayload = bar->getPayload();
  KJ_EXPECT(barPayload.isBar());
  KJ_EXPECT(kj::str(barPayload.getBar().getText()) == "t");
  KJ_EXPECT(barPayload.getBar().getPrimitives().getBool());

  auto etag = parse<::Tagged>(R"({"type": "etag", "value": "v", "kind": "is-weak"})", parseTagged);
  auto& etagPayload = etag->getPayload();
  KJ_EXPECT(etagPayload.which() == TaggedPayloadWhich::Etag);
  KJ_EXPECT(kj::str(etagPayload.getEtag().getValue()) == "v");
  KJ_EXPECT(etagPayload.getEtag().getKind().which() == EtagKindWhich::Weak);

  // A member without fields of its own has its default.
  auto empty = parse<::Tagged>(R"({"type": "bar"})", parseTagged);
  KJ_EXPECT(empty->getPayload().isBar());
  KJ_EXPECT(!empty->getPayload().getBar().hasText());

  auto etags = parse<::Etags>(
      R"({"etags": [{"value": "a", "kind": "strong"}, {"kind": "is-weak"}]})", parseEtags);
  KJ_ASSERT(etags->getEtags().size() == 2);
  KJ_EXPECT(etags->getEtags()[1].getKind().isWeak());
}

KJ_TEST("recursive values and nested declarations") {
  auto value = parse<::Recursive>(
      R"({"name": "root", "children": [{"name": "a", "children": [{"name": "b"}]}]})",
      parseRecursive);
  KJ_EXPECT(kj::str(value->getName()) == "root");
  auto names = KJ_MAP(element, value->getChildren()) { return kj::str(element.getName()); };
  KJ_EXPECT(kj::strArray(names, ",") == "a");

  auto outer = parse<::Outer>(R"({"inner": {"x": 1}, "count": 0})", parseOuter);
  KJ_EXPECT(outer->getInner().getX() == 1);
  KJ_EXPECT(outer->getCount() == 0);
  auto empty = parse<::Outer>("{}", parseOuter);
  KJ_EXPECT(!empty->hasInner());
  KJ_EXPECT(empty->getInner().getX() == 0);
  KJ_EXPECT(empty->getCount() == 3);

  auto inner = parse<::Outer::Inner>(R"({"x": 2})", parseOuterInner);
  KJ_EXPECT(inner->getX() == 2);
}

KJ_TEST("JSON that JsonCodec writes by default") {
  // JsonCodec's default HasMode::NON_NULL writes every field of a primitive type, the defaults
  // too. capnp-serde-json-test decodes these documents too.
  parse<::Primitives>(R"({"bool":true,"int8":-8,"int16":-16,"int32":-32,"int64":"-64",)"
                      R"("uint8":8,"uint16":16,"uint32":32,"uint64":"18446744073709551615"})",
      parsePrimitives);
  parse<::Primitives>(R"({"bool":false,"int8":0,"int16":0,"int32":0,"int64":"0","uint8":0,)"
                      R"("uint16":0,"uint32":0,"uint64":"0"})",
      parsePrimitives);
  parse<::Defaults>(R"({"bool":true,"int8":-8,"int64":"-64","uint32":4294967295,)"
                    R"("uint64":"18446744073709551615"})",
      parseDefaults);
  parse<::Defaults>(
      R"({"bool":false,"int8":0,"int64":"0","uint32":0,"uint64":"0"})", parseDefaults);
  parse<::Pointers>(
      R"({"text":"text","base64":"YWJj","hex":"616263","ints":["-1","2"],"shorts":[0,65535],)"
      R"("texts":["x","y"],"structs":[{"bool":false,"int8":1,"int16":0,"int32":0,"int64":"0",)"
      R"("uint8":0,"uint16":0,"uint32":0,"uint64":"0"},{"bool":false,"int8":0,"int16":0,)"
      R"("int32":0,"int64":"0","uint8":0,"uint16":0,"uint32":0,"uint64":"0"}],)"
      R"("primitives":{"bool":false,"int8":0,"int16":0,"int32":0,"int64":"0","uint8":0,)"
      R"("uint16":0,"uint32":5,"uint64":"0"},"not-a-rust-identifier":"renamed",)"
      R"("self":"self","match":"match"})",
      parsePointers);
  parse<::Pointers>("{}", parsePointers);
  parse<::Unions>(R"({"common":"c","a":3,"number":"5"})", parseUnions);
  parse<::Unions>(R"({"b":["x"],"string":"t"})", parseUnions);
  parse<::Unions>(R"({"a":0,"data":"6162"})", parseUnions);
  parse<::Unions>(R"({"a":0,"primitives":{"bool":false,"int8":0,"int16":7,"int32":0,)"
                  R"("int64":"0","uint8":0,"uint16":0,"uint32":0,"uint64":"0"}})",
      parseUnions);
  parse<::Unions>(R"({"a":0,"number":"0"})", parseUnions);
  parse<::Tagged>(R"({"id":1,"type":"foo"})", parseTagged);
  parse<::Tagged>(R"({"id":0,"type":"bar","text":"t","primitives":{"bool":true,"int8":0,)"
                  R"("int16":0,"int32":0,"int64":"0","uint8":0,"uint16":0,"uint32":0,)"
                  R"("uint64":"0"}})",
      parseTagged);
  parse<::Tagged>(R"({"id":0,"type":"etag","value":"v","kind":"is-weak"})", parseTagged);
  parse<::Etags>(R"({"etags":[{"value":"a","kind":"strong"},{"kind":"is-weak"}]})", parseEtags);
  parse<::Recursive>(
      R"({"name":"root","children":[{"name":"a","children":[{"name":"b"}]}]})", parseRecursive);
  parse<::Outer>(R"({"inner":{"x":1},"count":0})", parseOuter);
  parse<::Outer>(R"({"count":3})", parseOuter);
}

KJ_TEST("JsonCodec with HasMode::NON_DEFAULT writes what the Rust types write") {
  // The JSON capnp-serde-json-test expects of the Rust types.
  expectEncodes<::Primitives>(
      R"({"bool":true,"int8":-8,"int16":-16,"int32":-32,"int64":"-64","uint8":8,"uint16":16,)"
      R"("uint32":32,"uint64":"18446744073709551615"})");
  expectEncodes<::Primitives>("{}");
  expectEncodes<::Defaults>("{}");
  expectEncodes<::Defaults>(R"({"bool":false,"int8":0,"int64":"0","uint32":0,"uint64":"0"})");
  expectEncodes<::Pointers>(
      R"({"text":"text","base64":"YWJj","hex":"616263","ints":["-1","2"],"shorts":[0,65535],)"
      R"("texts":["x","y"],"structs":[{"int8":1},{}],"primitives":{"uint32":5},)"
      R"("not-a-rust-identifier":"renamed","self":"self","match":"match"})");
  expectEncodes<::Pointers>("{}");
  expectEncodes<::Unions>(R"({"common":"c","a":3,"number":"5"})");
  expectEncodes<::Unions>(R"({"b":["x"],"string":"t"})");
  expectEncodes<::Unions>(R"({"a":0,"data":"6162"})");
  expectEncodes<::Unions>(R"({"a":0,"primitives":{"int16":7}})");
  expectEncodes<::Unions>(R"({"a":0,"number":"0"})");
  expectEncodes<::Tagged>(R"({"id":1,"type":"foo"})");
  expectEncodes<::Tagged>(R"({"type":"bar","text":"t","primitives":{"bool":true}})");
  expectEncodes<::Tagged>(R"({"type":"etag","value":"v","kind":"is-weak"})");
  expectEncodes<::Etags>(R"({"etags":[{"value":"a","kind":"strong"},{"kind":"is-weak"}]})");
  expectEncodes<::Recursive>(
      R"({"name":"root","children":[{"name":"a","children":[{"name":"b"}]}]})");
  expectEncodes<::Outer>(R"({"inner":{"x":1},"count":0})");
  expectEncodes<::Outer>("{}");
}

KJ_TEST("JSON in other forms") {
  // Keys in any order, with whitespace.
  parse<::Primitives>(R"( { "uint64" : "7" , "bool" : true } )", parsePrimitives);
  parse<::Unions>(R"({"number": "5", "a": 1, "common": "c"})", parseUnions);
  // The tag of an internally tagged union may come after the member's fields.
  parse<::Tagged>(R"({"text": "t", "id": 2, "type": "bar"})", parseTagged);
  parse<::Tagged>(R"({"kind": "is-weak", "type": "etag", "value": "v"})", parseTagged);

  // 64-bit integers as numbers, as other writers write them. C++ reads every number as a double.
  parse<::Primitives>(R"({"int64": -5, "uint64": 9007199254740992})", parsePrimitives);

  // Unknown keys are ignored.
  parse<::Primitives>(R"({"unknown": 1, "int8": 1, "other": {"a": [1, 2]}})", parsePrimitives);
  parse<::Tagged>(R"({"type": "foo", "unknown": 1, "id": 4})", parseTagged);

  // null is a null pointer.
  parse<::Pointers>(R"({"text": null, "ints": null, "texts": null, "structs": null,
      "primitives": null, "self": null})",
      parsePointers);
}

KJ_TEST("invalid JSON throws") {
  KJ_EXPECT_THROW_MESSAGE("invalid type", parsePrimitives(bytes(R"({"int8": []})")));
  KJ_EXPECT_THROW_MESSAGE("EOF", parsePrimitives(bytes("{")));
  for (auto json: {"{", "1", R"({"int8": 1)", R"({"bool": "true"})", R"({"uint64": "-1"})"}) {
    KJ_EXPECT(rejects(json, parsePrimitives), json);
  }
  for (auto json: {R"({"type": "nope"})", "{}", R"({"type": true})"}) {
    KJ_EXPECT(rejects(json, parseTagged), json);
  }
}

}  // namespace
}  // namespace capnp_serde_json::test
