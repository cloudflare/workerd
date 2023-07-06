// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "assert";
import { test } from "node:test";
import {
  Member_Nested,
  Structure,
  StructureGroups,
  StructureGroups_StructureGroup,
  Type,
} from "@workerd/jsg/rtti.capnp.js";
import { Message } from "capnp-ts";
import { generateDefinitions } from "../../src/generator";
import { printNodeList } from "../../src/print";

// Initialises a structure group containing `targets` targets to reference.
// Returns a function to point a type at an identified target.
function initAsReferencableTypesGroup(
  group: StructureGroups_StructureGroup,
  targets: number
): (id: number, type: Type | Member_Nested) => void {
  group.setName("referenced");
  const structures = group.initStructures(targets);
  for (let i = 0; i < targets; i++) {
    const structure = structures.get(i);
    structure.setName(`Thing${i}`);
    structure.setFullyQualifiedName(`workerd::api::Thing${i}`);
  }
  return function initAsReferencedType(id, type) {
    assert(0 <= id && id < targets);
    const structure = type.initStructure();
    structure.setName(`Thing${id}`);
    structure.setFullyQualifiedName(`workerd::api::Thing${id}`);
  };
}

test("generateDefinitions: only includes referenced types from roots", () => {
  const root = new Message().initRoot(StructureGroups);
  const groups = root.initGroups(2);

  // Generate group containing definitions for referencing by next group
  const initAsReferencedType = initAsReferencableTypesGroup(groups.get(0), 24);

  // Generate group containing definitions with each possible type of visitable
  // type
  const group = groups.get(1);
  group.setName("definitions");
  const structures = group.initStructures(4);

  const root1 = structures.get(0);
  root1.setName("Root1");
  root1.setFullyQualifiedName("workerd::api::Root1");
  root1.setTsRoot(true);
  {
    const members = root1.initMembers(7);

    let prop = members.get(0).initProperty();
    prop.setName("promise");
    initAsReferencedType(0, prop.initType().initPromise().initValue());

    prop = members.get(1).initProperty();
    prop.setName("structure");
    initAsReferencedType(1, prop.initType());

    prop = members.get(2).initProperty();
    prop.setName("array");
    const array = prop.initType().initArray();
    array.setName("kj::Array");
    initAsReferencedType(2, array.initElement());

    prop = members.get(3).initProperty();
    prop.setName("maybe");
    const maybe = prop.initType().initMaybe();
    maybe.setName("jsg::Optional");
    initAsReferencedType(3, maybe.initValue());

    prop = members.get(4).initProperty();
    prop.setName("dict");
    const dict = prop.initType().initDict();
    initAsReferencedType(4, dict.initKey());
    initAsReferencedType(5, dict.initValue());

    prop = members.get(5).initProperty();
    prop.setName("variants");
    const variants = prop.initType().initOneOf().initVariants(3);
    initAsReferencedType(6, variants.get(0));
    initAsReferencedType(7, variants.get(1));
    initAsReferencedType(8, variants.get(2));

    prop = members.get(6).initProperty();
    prop.setName("function");
    const func = prop.initType().initFunction();
    initAsReferencedType(9, func.initArgs(1).get(0));
    initAsReferencedType(10, func.initReturnType());
  }

  const nested = structures.get(1);
  function initAsNestedStructure(structure: Structure) {
    structure.setName("Nested");
    structure.setFullyQualifiedName("workerd::api::Nested");
    const members = structure.initMembers(1);
    const prop = members.get(0).initProperty();
    prop.setName("nestedProp");
    initAsReferencedType(11, prop.initType());
  }
  initAsNestedStructure(nested);

  const root2 = structures.get(2);
  root2.setName("Root2");
  root2.setFullyQualifiedName("workerd::api::Root2");
  root2.setTsRoot(true);
  {
    const members = root2.initMembers(3);

    const method = members.get(0).initMethod();
    method.setName("method");
    initAsReferencedType(12, method.initArgs(1).get(0));
    initAsReferencedType(13, method.initReturnType());

    const nested = members.get(1).initNested();
    nested.setName("Nested");
    initAsNestedStructure(nested.initStructure());

    const constructor = members.get(2).initConstructor();
    initAsReferencedType(14, constructor.initArgs(1).get(0));
  }
  const iterator = root2.initIterator();
  initAsReferencedType(15, iterator.initArgs(1).get(0));
  initAsReferencedType(16, iterator.initReturnType());
  const asyncIterator = root2.initAsyncIterator();
  initAsReferencedType(17, asyncIterator.initArgs(1).get(0));
  initAsReferencedType(18, asyncIterator.initReturnType());
  initAsReferencedType(19, root2.initExtends());

  // Types referenced by non-roots shouldn't be included
  const nonRoot = structures.get(3);
  nonRoot.setName("NonRoot");
  nonRoot.setFullyQualifiedName("workerd::api::NonRoot");
  const members = nonRoot.initMembers(1);
  const prop = members.get(0).initProperty();
  prop.setName("nonRootProp");
  initAsReferencedType(20, prop.initType());

  const referencedInterfaces = Array.from(Array(19))
    .map((_, i) => `export interface Thing${i} {\n}`)
    .join("\n");
  const nodes = generateDefinitions(root);
  assert.strictEqual(
    printNodeList(nodes),
    `${referencedInterfaces}
export declare abstract class Thing19 {
}
export interface Root1 {
    promise: Promise<Thing0>;
    structure: Thing1;
    array: Thing2[];
    maybe?: Thing3;
    dict: Record<Thing4, Thing5>;
    variants: Thing6 | Thing7 | Thing8;
    function: (param0: Thing9) => Thing10;
}
export declare abstract class Nested {
    nestedProp: Thing11;
}
export declare class Root2 extends Thing19 {
    constructor(param0: Thing14);
    method(param0: Thing12): Thing13;
    Nested: typeof Nested;
    [Symbol.iterator](param0: Thing15): Thing16;
    [Symbol.asyncIterator](param0: Thing17): Thing18;
}
`
  );
});

test("generateDefinitions: only generates classes if required", () => {
  const root = new Message().initRoot(StructureGroups);
  const groups = root.initGroups(2);

  // Generate group containing definitions for referencing by next group
  const initAsReferencedType = initAsReferencableTypesGroup(groups.get(0), 2);

  // Generate group containing definitions with each possible class requirement
  const group = groups.get(1);
  group.setName("definitions");
  const structures = group.initStructures(4);

  const root1 = structures.get(0);
  root1.setName("Root1");
  root1.setFullyQualifiedName("workerd::api::Root1");
  root1.setTsRoot(true);
  // Thing0 should be a class as it's a nested type
  {
    const members = root1.initMembers(1);
    const nested = members.get(0).initNested();
    nested.setName("Thing0");
    initAsReferencedType(0, nested);
  }

  const root2 = structures.get(1);
  root2.setName("Root2");
  root2.setFullyQualifiedName("workerd::api::Root2");
  root2.setTsRoot(true);
  {
    const members = root2.initMembers(1);
    // ExportedHandler should be a class as it's constructible
    members.get(0).initConstructor();
  }

  const root3 = structures.get(2);
  root3.setName("Root3");
  root3.setFullyQualifiedName("workerd::api::Root3");
  root3.setTsRoot(true);
  {
    const members = root3.initMembers(1);
    const method = members.get(0).initMethod();
    method.setName("method");
    // DurableObjectNamespace should be a class as it contains static methods
    method.setStatic(true);
    method.initReturnType().setVoidt();
  }

  const root4 = structures.get(3);
  root4.setName("Root4");
  root4.setFullyQualifiedName("workerd::api::Root4");
  root4.setTsRoot(true);
  // Thing1 should be a class as its inherited
  initAsReferencedType(1, root4.initExtends());

  const nodes = generateDefinitions(root);
  assert.strictEqual(
    printNodeList(nodes),
    `export declare abstract class Thing0 {
}
export declare abstract class Thing1 {
}
export interface Root1 {
    Thing0: typeof Thing0;
}
export declare class Root2 {
    constructor();
}
export declare abstract class Root3 {
    static method(): void;
}
export interface Root4 extends Thing1 {
}
`
  );
});
