import assert from "assert";
import { test } from "node:test";
import {
  BuiltinType_Type,
  JsgImplType_Type,
  Type,
} from "@workerd/jsg/rtti.capnp.js";
import { Message } from "capnp-ts";
import { createTypeNode } from "../../src/generator/type";
import { printNode } from "../../src/print";

test("createTypeNode: primitive types", () => {
  const type = new Message().initRoot(Type);

  type.setUnknown();
  assert.strictEqual(printNode(createTypeNode(type)), "any");
  type.setObject();
  assert.strictEqual(printNode(createTypeNode(type)), "any");

  type.setVoidt();
  assert.strictEqual(printNode(createTypeNode(type)), "void");

  type.setBoolt();
  assert.strictEqual(printNode(createTypeNode(type)), "boolean");

  type.initNumber().setName("int");
  assert.strictEqual(printNode(createTypeNode(type)), "number");
  type.getNumber().setName("long");
  assert.strictEqual(printNode(createTypeNode(type)), "number | bigint");

  type.initString().setName("kj::String");
  assert.strictEqual(printNode(createTypeNode(type)), "string");

  const structure = type.initStructure();
  structure.setName("KvNamespace");
  structure.setFullyQualifiedName("workerd::api::KvNamespace");
  assert.strictEqual(printNode(createTypeNode(type)), "KvNamespace");
});

test("createTypeNode: builtin types", () => {
  const type = new Message().initRoot(Type);
  const builtin = type.initBuiltin();

  builtin.setType(BuiltinType_Type.V8UINT8ARRAY);
  assert.strictEqual(printNode(createTypeNode(type)), "Uint8Array");

  builtin.setType(BuiltinType_Type.V8ARRAY_BUFFER_VIEW);
  assert.strictEqual(printNode(createTypeNode(type)), "ArrayBufferView");

  builtin.setType(BuiltinType_Type.JSG_BUFFER_SOURCE);
  assert.strictEqual(
    printNode(createTypeNode(type)),
    "ArrayBuffer | ArrayBufferView"
  );

  builtin.setType(BuiltinType_Type.KJ_DATE);
  assert.strictEqual(printNode(createTypeNode(type)), "Date");

  builtin.setType(BuiltinType_Type.V8FUNCTION);
  assert.strictEqual(printNode(createTypeNode(type)), "Function");

  const intrinsic = type.initIntrinsic();

  intrinsic.setName("v8::kErrorPrototype");
  assert.strictEqual(printNode(createTypeNode(type)), "Error");

  intrinsic.setName("v8::kIteratorPrototype");
  assert.strictEqual(printNode(createTypeNode(type)), "Iterator<unknown>");

  intrinsic.setName("v8::kAsyncIteratorPrototype");
  assert.strictEqual(printNode(createTypeNode(type)), "AsyncIterator<unknown>");
});

test("createTypeNode: generic types", () => {
  const type = new Message().initRoot(Type);

  type.initPromise().initValue().setVoidt();
  assert.strictEqual(printNode(createTypeNode(type)), "Promise<void>");
  type.initPromise().initValue().setVoidt();
  assert.strictEqual(
    printNode(createTypeNode(type, true)),
    "void | Promise<void>"
  );
  assert.strictEqual(
    printNode(createTypeNode(type, true, true)),
    "Promise<any>"
  );

  const maybe = type.initMaybe();
  maybe.initValue().setBoolt();
  maybe.setName("jsg::Optional");
  assert.strictEqual(printNode(createTypeNode(type)), "boolean | undefined");
  maybe.setName("jsg::LenientOptional");
  assert.strictEqual(printNode(createTypeNode(type)), "boolean | undefined");
  maybe.setName("kj::Maybe");
  assert.strictEqual(printNode(createTypeNode(type)), "boolean | null");

  const dict = type.initDict();
  dict.initKey().initString().setName("kj::StringPtr");
  dict.initValue().initNumber().setName("short");
  assert.strictEqual(printNode(createTypeNode(type)), "Record<string, number>");

  const variants = type.initOneOf().initVariants(3);
  variants.get(0).setVoidt();
  variants.get(1).initNumber().setName("unsigned short");
  variants.get(2).initString().setName("kj::String");
  assert.strictEqual(printNode(createTypeNode(type)), "void | number | string");
});

test("createTypeNode: array types", () => {
  const type = new Message().initRoot(Type);
  const array = type.initArray();

  // Regular array
  array.setName("kj::Array");
  array.initElement().initString().setName("kj::String");
  assert.strictEqual(printNode(createTypeNode(type)), "string[]");
  assert.strictEqual(printNode(createTypeNode(type, true)), "string[]");
  // Iterable
  array.setName("jsg::Sequence");
  array.initElement().initString().setName("kj::String");
  assert.strictEqual(printNode(createTypeNode(type)), "string[]");
  assert.strictEqual(printNode(createTypeNode(type, true)), "Iterable<string>");

  // Numeric arrays
  array.setName("kj::Array");
  array.initElement().initNumber().setName("int");
  assert.strictEqual(printNode(createTypeNode(type)), "number[]");
  // If element is a byte, then this is an ArrayBuffer, ArrayBufferView or both
  array.setName("kj::Array");
  array.initElement().initNumber().setName("char");
  assert.strictEqual(printNode(createTypeNode(type)), "ArrayBuffer");
  assert.strictEqual(
    printNode(createTypeNode(type, true)),
    "ArrayBuffer | ArrayBufferView"
  );
  array.setName("kj::ArrayPtr");
  array.initElement().initNumber().setName("char");
  assert.strictEqual(printNode(createTypeNode(type)), "ArrayBufferView");
  assert.strictEqual(
    printNode(createTypeNode(type, true)),
    "ArrayBuffer | ArrayBufferView"
  );
});

test("createTypeNode: function types", () => {
  const message = new Message();

  // (a: boolean, b: number | undefined, d: string, c?: any) => void
  let type = message.initRoot(Type);
  let func = type.initFunction();
  let args = func.initArgs(5);
  args.get(0).setBoolt();
  args.get(1).initMaybe().initValue().initNumber().setName("int");
  args.get(2).initString().setName("kj::String");
  args.get(3).initMaybe().initValue().setObject();
  args.get(4).initJsgImpl().setType(JsgImplType_Type.V8ISOLATE);
  func.initReturnType().setVoidt();
  let typeNode = createTypeNode(type);
  assert.strictEqual(
    printNode(typeNode),
    "(param0: boolean, param1: number | undefined, param2: string, param3?: any) => void"
  );

  // (a?: string, ...b: any[]) => Promise<void>
  type = message.initRoot(Type);
  func = type.initFunction();
  args = func.initArgs(3);
  args.get(0).initJsgImpl().setType(JsgImplType_Type.JSG_TYPE_HANDLER);
  args.get(1).initMaybe().initValue().initString().setName("kj::String");
  args.get(2).initJsgImpl().setType(JsgImplType_Type.JSG_VARARGS);
  func.initReturnType().initPromise().initValue().setVoidt();
  typeNode = createTypeNode(type);
  assert.strictEqual(
    printNode(typeNode),
    "(param1?: string, ...param2: any[]) => void | Promise<void>"
  );
});

test("createTypeNode: implementation types", () => {
  const type = new Message().initRoot(Type);
  const impl = type.initJsgImpl();

  const implTypes: JsgImplType_Type[] = Object.values(type).filter(
    (member) => typeof member === "number"
  );
  for (const implType of implTypes) {
    // VARARGS and NAME are the only types we care about which will be tested
    // with function types, the rest should be ignored
    if (
      implType === JsgImplType_Type.JSG_VARARGS ||
      implType === JsgImplType_Type.JSG_NAME
    ) {
      continue;
    }
    impl.setType(implType);
    assert.strictEqual(printNode(createTypeNode(type)), "never");
  }

  impl.setType(JsgImplType_Type.JSG_NAME);
  assert.strictEqual(printNode(createTypeNode(type)), "PropertyKey");
});
