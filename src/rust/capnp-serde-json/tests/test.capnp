# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

# Every schema construct capnpc-rust-serde-json supports, for capnp-serde-json-test.rs and
# capnp-serde-json-ffi-test.c++.

@0xf079d8d97f6fdf70;

using Json = import "/capnp/compat/json.capnp";

const answer :Int32 = -42;
const big :UInt64 = 0xffffffffffffffff;
const yes :Bool = true;

enum Color {
  # Enums are not generated, but a schema may declare them.
  red @0;
  blue @1;
}

struct Primitives {
  bool @0 :Bool;
  int8 @1 :Int8;
  int16 @2 :Int16;
  int32 @3 :Int32;
  int64 @4 :Int64;
  uint8 @5 :UInt8;
  uint16 @6 :UInt16;
  uint32 @7 :UInt32;
  uint64 @8 :UInt64;
}

struct Defaults {
  bool @0 :Bool = true;
  int8 @1 :Int8 = -8;
  int64 @2 :Int64 = -64;
  uint32 @3 :UInt32 = 0xffffffff;
  uint64 @4 :UInt64 = 0xffffffffffffffff;
}

struct Pointers {
  text @0 :Text;
  base64 @1 :Data $Json.base64;
  hex @2 :Data $Json.hex;
  ints @3 :List(Int64);
  shorts @4 :List(UInt16);
  texts @5 :List(Text);
  structs @6 :List(Primitives);
  primitives @7 :Primitives;
  renamed @8 :Text $Json.name("not-a-rust-identifier");
  self @9 :Text;
  match @10 :Text;
}

struct Unions {
  common @0 :Text;

  union {
    # Externally tagged and flattened into the struct.
    number @1 :UInt64;
    text @2 :Text $Json.name("string");
    data @3 :Data $Json.hex;
    primitives @4 :Primitives;
  }

  named :union $Json.flatten() {
    # Externally tagged and flattened into the struct too.
    a @5 :Int32;
    b @6 :List(Text);
  }
}

struct Tagged {
  id @0 :UInt32;

  payload :union $Json.flatten() $Json.discriminator(name = "type") {
    # Internally tagged by "type", with the members' fields next to it.
    foo @1 :Void;
    bar @2 :Pointers $Json.flatten();
    etag @3 :Etag $Json.flatten();
  }
}

struct Etag {
  value @0 :Text;

  kind :union $Json.flatten() $Json.discriminator(name = "kind") {
    strong @1 :Void;
    weak @2 :Void $Json.name("is-weak");
  }
}

struct Etags {
  etags @0 :List(Etag);
}

struct Recursive {
  name @0 :Text;
  children @1 :List(Recursive);
}

struct Outer {
  # A struct with nested declarations.

  struct Inner {
    x @0 :Int32;
  }

  enum Mode {
    on @0;
    off @1;
  }

  const limit :UInt32 = 7;

  inner @0 :Inner;
  count @1 :UInt32 = 3;
}
