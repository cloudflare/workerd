// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'assert';
import { test } from 'node:test';
import { JsgImplType_Type, Structure } from '@workerd/jsg/rtti';
import { Message } from 'capnp-es';
import { createStructureNode } from '../../src/generator/structure';
import { printNode } from '../../src/print';

test('createStructureNode: method members', () => {
  const structure = new Message().initRoot(Structure);
  structure.name = 'Methods';
  structure.fullyQualifiedName = 'workerd::api::Methods';

  const members = structure._initMembers(3);

  let method = members.get(0)._initMethod();
  method.name = 'one';
  {
    const args = method._initArgs(3);
    args.get(0).boolt = true;
    args.get(1)._initPromise()._initValue().voidt = true;
    const maybe = args.get(2)._initMaybe();
    maybe.name = 'jsg::Optional';
    maybe._initValue()._initNumber().name = 'int';
  }
  method._initReturnType().voidt = true;

  method = members.get(1)._initMethod();
  method.name = 'two';
  method._initReturnType()._initJsgImpl().type =
    JsgImplType_Type.JSG_UNIMPLEMENTED;

  method = members.get(2)._initMethod();
  method.name = 'three';
  {
    const args = method._initArgs(1);
    args.get(0)._initJsgImpl().type = JsgImplType_Type.JSG_VARARGS;
  }
  method._initReturnType().boolt = true;

  // Note method with unimplemented return is omitted
  assert.strictEqual(
    printNode(createStructureNode(structure, { asClass: false })),
    `interface Methods {
    one(param0: boolean, param1: Promise<any>, param2?: number): void;
    three(...param0: any[]): boolean;
}`
  );

  method.static = true;
  assert.strictEqual(
    printNode(createStructureNode(structure, { asClass: true })),
    `declare abstract class Methods {
    one(param0: boolean, param1: Promise<any>, param2?: number): void;
    static three(...param0: any[]): boolean;
}`
  );
});

test('createStructureNode: property members', () => {
  const structure = new Message().initRoot(Structure);
  structure.name = 'Properties';
  structure.fullyQualifiedName = 'workerd::api::Properties';

  const members = structure._initMembers(8);

  let prop = members.get(0)._initProperty();
  prop.name = 'one';
  prop._initType().boolt = true;

  prop = members.get(1)._initProperty();
  prop.name = 'two';
  prop._initType()._initNumber().name = 'int';
  prop.readonly = true;

  prop = members.get(2)._initProperty();
  prop.name = 'three';
  {
    const maybe = prop._initType()._initMaybe();
    maybe.name = 'jsg::Optional';
    maybe._initValue().boolt = true;
  }
  prop.readonly = true;

  prop = members.get(3)._initProperty();
  prop.name = 'four';
  prop._initType()._initJsgImpl().type = JsgImplType_Type.JSG_UNIMPLEMENTED;
  prop.readonly = true;

  prop = members.get(4)._initProperty();
  prop.name = 'five';
  prop._initType().boolt = true;
  prop.prototype = true;

  prop = members.get(5)._initProperty();
  prop.name = 'six';
  prop._initType()._initString().name = 'kj::String';
  prop.readonly = true;
  prop.prototype = true;

  prop = members.get(6)._initProperty();
  prop.name = 'seven';
  {
    const maybe = prop._initType()._initMaybe();
    maybe.name = 'jsg::Optional';
    maybe._initValue()._initNumber().name = 'short';
  }
  prop.readonly = true;
  prop.prototype = true;

  prop = members.get(7)._initProperty();
  prop.name = 'eight';
  prop._initType()._initJsgImpl().type = JsgImplType_Type.JSG_UNIMPLEMENTED;
  prop.readonly = true;
  prop.prototype = true;

  // Note unimplemented properties omitted
  assert.strictEqual(
    printNode(createStructureNode(structure, { asClass: false })),
    `interface Properties {
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
    printNode(createStructureNode(structure, { asClass: true })),
    `declare abstract class Properties {
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

test('createStructureNode: nested type members', () => {
  const structure = new Message().initRoot(Structure);
  structure.name = 'Nested';
  structure.fullyQualifiedName = 'workerd::api::Nested';

  const members = structure._initMembers(2);

  let nested = members.get(0)._initNested();
  let nestedStructure = nested._initStructure();
  nestedStructure.name = 'Thing';
  nestedStructure.fullyQualifiedName = 'workerd::api::Thing';

  nested = members.get(1)._initNested();
  nestedStructure = nested._initStructure();
  nestedStructure.name = 'OtherThing';
  nestedStructure.fullyQualifiedName = 'workerd::api::OtherThing';
  nested.name = 'RenamedThing';

  assert.strictEqual(
    printNode(createStructureNode(structure, { asClass: false })),
    `interface Nested {
    Thing: typeof Thing;
    RenamedThing: typeof OtherThing;
}`
  );
  assert.strictEqual(
    printNode(createStructureNode(structure, { asClass: true })),
    `declare abstract class Nested {
    Thing: typeof Thing;
    RenamedThing: typeof OtherThing;
}`
  );
});

test('createStructureNode: constant members', () => {
  const structure = new Message().initRoot(Structure);
  structure.name = 'Constants';
  structure.fullyQualifiedName = 'workerd::api::Constants';

  const members = structure._initMembers(1);

  const constant = members.get(0)._initConstant();
  constant.name = 'THING';
  constant.value = BigInt(42);

  assert.strictEqual(
    printNode(createStructureNode(structure, { asClass: true })),
    `declare abstract class Constants {
    static readonly THING: number;
}`
  );
});

test('createStructureNode: iterator members', () => {
  const structure = new Message().initRoot(Structure);
  structure.name = 'Iterators';
  structure.fullyQualifiedName = 'workerd::api::Iterators';

  structure.iterable = true;
  const iterator = structure._initIterator();
  {
    const args = iterator._initArgs(1);
    const maybe = args.get(0)._initMaybe();
    maybe.name = 'jsg::Optional';
    const optionsStructure = maybe._initValue()._initStructure();
    optionsStructure.name = 'ThingOptions';
    optionsStructure.fullyQualifiedName = 'workerd::api::ThingOptions';
  }
  let returnStructure = iterator._initReturnType()._initStructure();
  returnStructure.name = 'ThingIterator';
  returnStructure.fullyQualifiedName = 'workerd::api::ThingIterator';

  structure.asyncIterable = true;
  const asyncIterator = structure._initAsyncIterator();
  {
    const args = asyncIterator._initArgs(1);
    const maybe = args.get(0)._initMaybe();
    maybe.name = 'jsg::Optional';
    const optionsStructure = maybe._initValue()._initStructure();
    optionsStructure.name = 'AsyncThingOptions';
    optionsStructure.fullyQualifiedName = 'workerd::api::AsyncThingOptions';
  }
  returnStructure = asyncIterator._initReturnType()._initStructure();
  returnStructure.name = 'AsyncThingIterator';
  returnStructure.fullyQualifiedName = 'workerd::api::AsyncThingIterator';

  assert.strictEqual(
    printNode(createStructureNode(structure, { asClass: false })),
    `interface Iterators {
    [Symbol.iterator](param0?: ThingOptions): ThingIterator;
    [Symbol.asyncIterator](param0?: AsyncThingOptions): AsyncThingIterator;
}`
  );
  assert.strictEqual(
    printNode(createStructureNode(structure, { asClass: true })),
    `declare abstract class Iterators {
    [Symbol.iterator](param0?: ThingOptions): ThingIterator;
    [Symbol.asyncIterator](param0?: AsyncThingOptions): AsyncThingIterator;
}`
  );
});

test('createStructureNode: constructors', () => {
  const structure = new Message().initRoot(Structure);
  structure.name = 'Constructor';
  structure.fullyQualifiedName = 'workerd::api::Constructor';

  const members = structure._initMembers(1);

  const constructor = members.get(0)._initConstructor();
  {
    const args = constructor._initArgs(4);
    let maybe = args.get(0)._initMaybe();
    maybe.name = 'jsg::Optional';
    maybe._initValue().boolt = true;
    args.get(1)._initString().name = 'kj::String';
    args.get(2)._initPromise()._initValue().voidt = true;
    maybe = args.get(3)._initMaybe();
    maybe.name = 'jsg::Optional';
    maybe._initValue()._initNumber().name = 'int';
  }

  assert.strictEqual(
    printNode(createStructureNode(structure, { asClass: true })),
    `declare class Constructor {
    constructor(param0: boolean | undefined, param1: string, param2: Promise<any>, param3?: number);
}`
  );
});

test('createStructureNode: extends', () => {
  const structure = new Message().initRoot(Structure);
  structure.name = 'Extends';
  structure.fullyQualifiedName = 'workerd::api::Extends';

  const extendsStructure = structure._initExtends()._initStructure();
  extendsStructure.name = 'Base';
  extendsStructure.fullyQualifiedName = 'workerd::api::Base';

  assert.strictEqual(
    printNode(createStructureNode(structure, { asClass: true })),
    `declare abstract class Extends extends Base {
}`
  );
});
