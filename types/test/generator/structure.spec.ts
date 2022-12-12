import assert from "assert";
import { test } from "node:test";
import { JsgImplType_Type, Structure } from "@workerd/jsg/rtti.capnp.js";
import { Int64, Message } from "capnp-ts";
import { createStructureNode } from "../../src/generator/structure";
import { printNode } from "../../src/print";

test("createStructureNode: method members", () => {
  const structure = new Message().initRoot(Structure);
  structure.setName("Methods");
  structure.setFullyQualifiedName("workerd::api::Methods");

  const members = structure.initMembers(3);

  let method = members.get(0).initMethod();
  method.setName("one");
  {
    const args = method.initArgs(3);
    args.get(0).setBoolt();
    args.get(1).initPromise().initValue().setVoidt();
    const maybe = args.get(2).initMaybe();
    maybe.setName("jsg::Optional");
    maybe.initValue().initNumber().setName("int");
  }
  method.initReturnType().setVoidt();

  method = members.get(1).initMethod();
  method.setName("two");
  method
    .initReturnType()
    .initJsgImpl()
    .setType(JsgImplType_Type.JSG_UNIMPLEMENTED);

  method = members.get(2).initMethod();
  method.setName("three");
  {
    const args = method.initArgs(1);
    args.get(0).initJsgImpl().setType(JsgImplType_Type.JSG_VARARGS);
  }
  method.initReturnType().setBoolt();

  // Note method with unimplemented return is omitted
  assert.strictEqual(
    printNode(createStructureNode(structure, false)),
    `export interface Methods {
    one(param0: boolean, param1: Promise<any>, param2?: number): void;
    three(...param0: any[]): boolean;
}`
  );

  method.setStatic(true);
  assert.strictEqual(
    printNode(createStructureNode(structure, true)),
    `export declare abstract class Methods {
    one(param0: boolean, param1: Promise<any>, param2?: number): void;
    static three(...param0: any[]): boolean;
}`
  );
});

test("createStructureNode: property members", () => {
  const structure = new Message().initRoot(Structure);
  structure.setName("Properties");
  structure.setFullyQualifiedName("workerd::api::Properties");

  const members = structure.initMembers(8);

  let prop = members.get(0).initProperty();
  prop.setName("one");
  prop.initType().setBoolt();

  prop = members.get(1).initProperty();
  prop.setName("two");
  prop.initType().initNumber().setName("int");
  prop.setReadonly(true);

  prop = members.get(2).initProperty();
  prop.setName("three");
  {
    const maybe = prop.initType().initMaybe();
    maybe.setName("jsg::Optional");
    maybe.initValue().setBoolt();
  }
  prop.setReadonly(true);

  prop = members.get(3).initProperty();
  prop.setName("four");
  prop.initType().initJsgImpl().setType(JsgImplType_Type.JSG_UNIMPLEMENTED);
  prop.setReadonly(true);

  prop = members.get(4).initProperty();
  prop.setName("five");
  prop.initType().setBoolt();
  prop.setPrototype(true);

  prop = members.get(5).initProperty();
  prop.setName("six");
  prop.initType().initString().setName("kj::String");
  prop.setReadonly(true);
  prop.setPrototype(true);

  prop = members.get(6).initProperty();
  prop.setName("seven");
  {
    const maybe = prop.initType().initMaybe();
    maybe.setName("jsg::Optional");
    maybe.initValue().initNumber().setName("short");
  }
  prop.setReadonly(true);
  prop.setPrototype(true);

  prop = members.get(7).initProperty();
  prop.setName("eight");
  prop.initType().initJsgImpl().setType(JsgImplType_Type.JSG_UNIMPLEMENTED);
  prop.setReadonly(true);
  prop.setPrototype(true);

  // Note unimplemented properties omitted
  assert.strictEqual(
    printNode(createStructureNode(structure, false)),
    `export interface Properties {
    one: boolean;
    readonly two: number;
    readonly three?: boolean;
    get five(): boolean;
    set five(value: boolean);
    get six(): string;
    get seven(): number | undefined;
}`
  );
  assert.strictEqual(
    printNode(createStructureNode(structure, true)),
    `export declare abstract class Properties {
    one: boolean;
    readonly two: number;
    readonly three?: boolean;
    get five(): boolean;
    set five(value: boolean);
    get six(): string;
    get seven(): number | undefined;
}`
  );
});

test("createStructureNode: nested type members", () => {
  const structure = new Message().initRoot(Structure);
  structure.setName("Nested");
  structure.setFullyQualifiedName("workerd::api::Nested");

  const members = structure.initMembers(2);

  let nested = members.get(0).initNested();
  let nestedStructure = nested.initStructure();
  nestedStructure.setName("Thing");
  nestedStructure.setFullyQualifiedName("workerd::api::Thing");

  nested = members.get(1).initNested();
  nestedStructure = nested.initStructure();
  nestedStructure.setName("OtherThing");
  nestedStructure.setFullyQualifiedName("workerd::api::OtherThing");
  nested.setName("RenamedThing");

  assert.strictEqual(
    printNode(createStructureNode(structure, false)),
    `export interface Nested {
    Thing: typeof Thing;
    RenamedThing: typeof OtherThing;
}`
  );
  assert.strictEqual(
    printNode(createStructureNode(structure, true)),
    `export declare abstract class Nested {
    Thing: typeof Thing;
    RenamedThing: typeof OtherThing;
}`
  );
});

test("createStructureNode: constant members", () => {
  const structure = new Message().initRoot(Structure);
  structure.setName("Constants");
  structure.setFullyQualifiedName("workerd::api::Constants");

  const members = structure.initMembers(1);

  const constant = members.get(0).initConstant();
  constant.setName("THING");
  constant.setValue(Int64.fromNumber(42));

  assert.strictEqual(
    printNode(createStructureNode(structure, true)),
    `export declare abstract class Constants {
    static readonly THING: number;
}`
  );
});

test("createStructureNode: iterator members", () => {
  const structure = new Message().initRoot(Structure);
  structure.setName("Iterators");
  structure.setFullyQualifiedName("workerd::api::Iterators");

  structure.setIterable(true);
  const iterator = structure.initIterator();
  {
    const args = iterator.initArgs(1);
    const maybe = args.get(0).initMaybe();
    maybe.setName("jsg::Optional");
    const optionsStructure = maybe.initValue().initStructure();
    optionsStructure.setName("ThingOptions");
    optionsStructure.setFullyQualifiedName("workerd::api::ThingOptions");
  }
  let returnStructure = iterator.initReturnType().initStructure();
  returnStructure.setName("ThingIterator");
  returnStructure.setFullyQualifiedName("workerd::api::ThingIterator");

  structure.setAsyncIterable(true);
  const asyncIterator = structure.initAsyncIterator();
  {
    const args = asyncIterator.initArgs(1);
    const maybe = args.get(0).initMaybe();
    maybe.setName("jsg::Optional");
    const optionsStructure = maybe.initValue().initStructure();
    optionsStructure.setName("AsyncThingOptions");
    optionsStructure.setFullyQualifiedName("workerd::api::AsyncThingOptions");
  }
  returnStructure = asyncIterator.initReturnType().initStructure();
  returnStructure.setName("AsyncThingIterator");
  returnStructure.setFullyQualifiedName("workerd::api::AsyncThingIterator");

  assert.strictEqual(
    printNode(createStructureNode(structure, false)),
    `export interface Iterators {
    [Symbol.iterator](param0?: ThingOptions): ThingIterator;
    [Symbol.asyncIterator](param0?: AsyncThingOptions): AsyncThingIterator;
}`
  );
  assert.strictEqual(
    printNode(createStructureNode(structure, true)),
    `export declare abstract class Iterators {
    [Symbol.iterator](param0?: ThingOptions): ThingIterator;
    [Symbol.asyncIterator](param0?: AsyncThingOptions): AsyncThingIterator;
}`
  );
});

test("createStructureNode: constructors", () => {
  const structure = new Message().initRoot(Structure);
  structure.setName("Constructor");
  structure.setFullyQualifiedName("workerd::api::Constructor");

  const members = structure.initMembers(1);

  const constructor = members.get(0).initConstructor();
  {
    const args = constructor.initArgs(4);
    let maybe = args.get(0).initMaybe();
    maybe.setName("jsg::Optional");
    maybe.initValue().setBoolt();
    args.get(1).initString().setName("kj::String");
    args.get(2).initPromise().initValue().setVoidt();
    maybe = args.get(3).initMaybe();
    maybe.setName("jsg::Optional");
    maybe.initValue().initNumber().setName("int");
  }

  assert.strictEqual(
    printNode(createStructureNode(structure, true)),
    `export declare class Constructor {
    constructor(param0: boolean | undefined, param1: string, param2: Promise<any>, param3?: number);
}`
  );
});

test("createStructureNode: extends", () => {
  const structure = new Message().initRoot(Structure);
  structure.setName("Extends");
  structure.setFullyQualifiedName("workerd::api::Extends");

  const extendsStructure = structure.initExtends().initStructure();
  extendsStructure.setName("Base");
  extendsStructure.setFullyQualifiedName("workerd::api::Base");

  assert.strictEqual(
    printNode(createStructureNode(structure, true)),
    `export declare abstract class Extends extends Base {
}`
  );
});
