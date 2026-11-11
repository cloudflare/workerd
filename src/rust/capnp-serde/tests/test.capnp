# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

# Every schema construct capnpc-rust-serde supports, for capnp-serde-test.rs.

@0xf079d8d97f6fdf70;

using Json = import "/capnp/compat/json.capnp";

const answer :Int32 = -42;
const greeting :Text = "hello \"world\"";
const pi :Float64 = 3.25;
const blob :Data = 0x"0102";
const favorite :Color = darkBlue;

enum Color {
  red @0;
  darkBlue @1 $Json.name("blue");
  lightGreen @2;
}

struct Primitives {
  void @0 :Void;
  bool @1 :Bool;
  int8 @2 :Int8;
  int16 @3 :Int16;
  int32 @4 :Int32;
  int64 @5 :Int64;
  uint8 @6 :UInt8;
  uint16 @7 :UInt16;
  uint32 @8 :UInt32;
  uint64 @9 :UInt64;
  float32 @10 :Float32;
  float64 @11 :Float64;
  color @12 :Color;
}

struct Defaults {
  bool @0 :Bool = true;
  int8 @1 :Int8 = -8;
  int64 @2 :Int64 = -64;
  uint64 @3 :UInt64 = 0xffffffffffffffff;
  float32 @4 :Float32 = 1.5;
  float64 @5 :Float64 = inf;
  nan @6 :Float64 = nan;
  color @7 :Color = lightGreen;
}

struct Pointers {
  text @0 :Text;
  data @1 :Data;
  base64 @2 :Data $Json.base64;
  hex @3 :Data $Json.hex;
  ints @4 :List(Int64);
  bytes @5 :List(UInt8);
  datas @6 :List(Data);
  texts @7 :List(Text);
  colors @8 :List(Color);
  structs @9 :List(Primitives);
  nested @10 :List(List(Float32));
  primitives @11 :Primitives;
  json @12 :Json.Value;
  renamed @13 :Text $Json.name("not-a-rust-identifier");
  self @14 :Text;
  match @15 :Text;
}

struct Unions {
  common @0 :Text;

  union {
    # Externally tagged and flattened into the struct.
    none @1 :Void;
    number @2 :UInt64;
    text @3 :Text $Json.name("string");
    data @4 :Data $Json.hex;
  }

  plain :union {
    # Externally tagged, in an object of its own.
    a @5 :Int32;
    b @6 :Void;
  }

  tagged :union $Json.discriminator(name = "kind") {
    # Internally tagged, in an object of its own.
    x @7 :Int32;
    y @8 :Void;
    z @9 :Primitives $Json.flatten();
  }

  flat :union $Json.flatten() $Json.discriminator(valueName = "value") {
    # Internally tagged by "flat", the union's name, and flattened into the struct.
    p @10 :Text;
    q @11 :Float64 $Json.name("renamed-q");
  }

  grouped :group {
    inGroup @12 :Text;
    union {
      g1 @13 :Text;
      g2 @14 :Void;
    }
  }

  flatGroup :group $Json.flatten() {
    flatMember @15 :UInt32;
  }
}

struct Discriminated $Json.discriminator(name = "type") {
  union {
    foo @0 :UInt32;
    bar @1 :Pointers $Json.flatten();
    baz :group $Json.flatten() {
      bazMember @2 :Text;
    }
    qux @3 :Void;
  }
}

struct Recursive {
  name @0 :Text;
  child @1 :Recursive;
  children @2 :List(Recursive);
  cycle :union {
    none @3 :Void;
    other @4 :Recursive;
  }
}

struct Flattened {
  before @0 :Text;
  inner @1 :Primitives $Json.flatten();
  after @2 :Text;
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
  mode @1 :Mode = off;
}

struct Boxed $Json.discriminator(name = "kind") {
  # Flattened values that hold the struct they are flattened into, and so are boxed.
  inner @0 :BoxedInner $Json.flatten();
  union {
    member @1 :BoxedMember $Json.flatten();
    none @2 :Void;
  }
}

struct BoxedInner {
  name @0 :Text;
  outer @1 :Boxed;
}

struct BoxedMember {
  label @0 :Text;
  parent @1 :Boxed;
}
