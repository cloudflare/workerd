// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert'
import { test } from 'node:test'
import {
  type Member_Nested,
  type Structure,
  StructureGroups,
  type StructureGroups_StructureGroup,
  type Type,
} from '@workerd/jsg/rtti'
import { Message } from 'capnp-es'
import { generateDefinitions } from '../../src/generator'
import { printNodeList } from '../../src/print'

// Initializes a structure group containing `targets` targets to reference.
// Returns a function to point a type at an identified target.
function initAsReferencableTypesGroup(
  group: StructureGroups_StructureGroup,
  targets: number,
): (id: number, type: Type | Member_Nested) => void {
  group.name = 'referenced'
  const structures = group._initStructures(targets)
  for (let i = 0; i < targets; i++) {
    const structure = structures.get(i)
    structure.name = `Thing${i}`
    structure.fullyQualifiedName = `workerd::api::Thing${i}`
  }
  return function initAsReferencedType(id, type) {
    assert(0 <= id && id < targets)
    const structure = type._initStructure()
    structure.name = `Thing${id}`
    structure.fullyQualifiedName = `workerd::api::Thing${id}`
  }
}

test('generateDefinitions: only includes referenced types from roots', () => {
  const root = new Message().initRoot(StructureGroups)
  const groups = root._initGroups(2)

  // Generate group containing definitions for referencing by next group
  const initAsReferencedType = initAsReferencableTypesGroup(groups.get(0), 24)

  // Generate group containing definitions with each possible type of visitable
  // type
  const group = groups.get(1)
  group.name = 'definitions'
  const structures = group._initStructures(4)

  const root1 = structures.get(0)
  root1.name = 'Root1'
  root1.fullyQualifiedName = 'workerd::api::Root1'
  root1.tsRoot = true
  {
    const members = root1._initMembers(7)

    let prop = members.get(0)._initProperty()
    prop.name = 'promise'
    initAsReferencedType(0, prop._initType()._initPromise()._initValue())

    prop = members.get(1)._initProperty()
    prop.name = 'structure'
    initAsReferencedType(1, prop._initType())

    prop = members.get(2)._initProperty()
    prop.name = 'array'
    const array = prop._initType()._initArray()
    array.name = 'kj::Array'
    initAsReferencedType(2, array._initElement())

    prop = members.get(3)._initProperty()
    prop.name = 'maybe'
    const maybe = prop._initType()._initMaybe()
    maybe.name = 'jsg::Optional'
    initAsReferencedType(3, maybe._initValue())

    prop = members.get(4)._initProperty()
    prop.name = 'dict'
    const dict = prop._initType()._initDict()
    initAsReferencedType(4, dict._initKey())
    initAsReferencedType(5, dict._initValue())

    prop = members.get(5)._initProperty()
    prop.name = 'variants'
    const variants = prop._initType()._initOneOf()._initVariants(3)
    initAsReferencedType(6, variants.get(0))
    initAsReferencedType(7, variants.get(1))
    initAsReferencedType(8, variants.get(2))

    prop = members.get(6)._initProperty()
    prop.name = 'function'
    const func = prop._initType()._initFunction()
    initAsReferencedType(9, func._initArgs(1).get(0))
    initAsReferencedType(10, func._initReturnType())
  }

  const nested = structures.get(1)
  function initAsNestedStructure(structure: Structure): void {
    structure.name = 'Nested'
    structure.fullyQualifiedName = 'workerd::api::Nested'
    const members = structure._initMembers(1)
    const prop = members.get(0)._initProperty()
    prop.name = 'nestedProp'
    initAsReferencedType(11, prop._initType())
  }
  initAsNestedStructure(nested)

  const root2 = structures.get(2)
  root2.name = 'Root2'
  root2.fullyQualifiedName = 'workerd::api::Root2'
  root2.tsRoot = true
  {
    const members = root2._initMembers(3)

    const method = members.get(0)._initMethod()
    method.name = 'method'
    initAsReferencedType(12, method._initArgs(1).get(0))
    initAsReferencedType(13, method._initReturnType())

    const nested = members.get(1)._initNested()
    nested.name = 'Nested'
    initAsNestedStructure(nested._initStructure())

    const constructor = members.get(2)._initConstructor()
    initAsReferencedType(14, constructor._initArgs(1).get(0))
  }
  const iterator = root2._initIterator()
  initAsReferencedType(15, iterator._initArgs(1).get(0))
  initAsReferencedType(16, iterator._initReturnType())
  const asyncIterator = root2._initAsyncIterator()
  initAsReferencedType(17, asyncIterator._initArgs(1).get(0))
  initAsReferencedType(18, asyncIterator._initReturnType())
  initAsReferencedType(19, root2._initExtends())

  // Types referenced by non-roots shouldn't be included
  const nonRoot = structures.get(3)
  nonRoot.name = 'NonRoot'
  nonRoot.fullyQualifiedName = 'workerd::api::NonRoot'
  const members = nonRoot._initMembers(1)
  const prop = members.get(0)._initProperty()
  prop.name = 'nonRootProp'
  initAsReferencedType(20, prop._initType())

  const referencedInterfaces = Array.from(Array(19))
    .map((_, i) => `interface Thing${i} {\n}`)
    .join('\n')
  const { nodes } = generateDefinitions(root)
  assert.strictEqual(
    printNodeList(nodes),
    `${referencedInterfaces}
declare abstract class Thing19 {
}
interface Root1 {
    promise: Promise<Thing0>;
    structure: Thing1;
    array: Thing2[];
    maybe?: Thing3;
    dict: Record<Thing4, Thing5>;
    variants: Thing6 | Thing7 | Thing8;
    function: (param0: Thing9) => Thing10;
}
declare abstract class Nested {
    nestedProp: Thing11;
}
declare class Root2 extends Thing19 {
    constructor(param0: Thing14);
    method(param0: Thing12): Thing13;
    Nested: typeof Nested;
    [Symbol.iterator](param0: Thing15): Thing16;
    [Symbol.asyncIterator](param0: Thing17): Thing18;
}
`,
  )
})

test('generateDefinitions: only generates classes if required', () => {
  const root = new Message().initRoot(StructureGroups)
  const groups = root._initGroups(2)

  // Generate group containing definitions for referencing by next group
  const initAsReferencedType = initAsReferencableTypesGroup(groups.get(0), 2)

  // Generate group containing definitions with each possible class requirement
  const group = groups.get(1)
  group.name = 'definitions'
  const structures = group._initStructures(4)

  const root1 = structures.get(0)
  root1.name = 'Root1'
  root1.fullyQualifiedName = 'workerd::api::Root1'
  root1.tsRoot = true
  // Thing0 should be a class as it's a nested type
  {
    const members = root1._initMembers(1)
    const nested = members.get(0)._initNested()
    nested.name = 'Thing0'
    initAsReferencedType(0, nested)
  }

  const root2 = structures.get(1)
  root2.name = 'Root2'
  root2.fullyQualifiedName = 'workerd::api::Root2'
  root2.tsRoot = true
  {
    const members = root2._initMembers(1)
    // ExportedHandler should be a class as it's constructible
    members.get(0)._initConstructor()
  }

  const root3 = structures.get(2)
  root3.name = 'Root3'
  root3.fullyQualifiedName = 'workerd::api::Root3'
  root3.tsRoot = true
  {
    const members = root3._initMembers(1)
    const method = members.get(0)._initMethod()
    method.name = 'method'
    // DurableObjectNamespace should be a class as it contains static methods
    method.static = true
    method._initReturnType().voidt = true
  }

  const root4 = structures.get(3)
  root4.name = 'Root4'
  root4.fullyQualifiedName = 'workerd::api::Root4'
  root4.tsRoot = true
  // Thing1 should be a class as its inherited
  initAsReferencedType(1, root4._initExtends())

  const { nodes } = generateDefinitions(root)
  assert.strictEqual(
    printNodeList(nodes),
    `declare abstract class Thing0 {
}
declare abstract class Thing1 {
}
interface Root1 {
    Thing0: typeof Thing0;
}
declare class Root2 {
    constructor();
}
declare abstract class Root3 {
    static method(): void;
}
interface Root4 extends Thing1 {
}
`,
  )
})
