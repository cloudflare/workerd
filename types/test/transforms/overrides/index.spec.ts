// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert'
import path from 'node:path'
import { test } from 'node:test'
import {
  type Member_Nested,
  StructureGroups,
  type Type,
} from '@workerd/jsg/rtti'
import { Message } from 'capnp-es'
import ts from 'typescript'
import { generateDefinitions } from '../../../src/generator'
import { printer, printNodeList } from '../../../src/print'
import { createMemoryProgram } from '../../../src/program'
import {
  compileOverridesDefines,
  createOverrideDefineTransformer,
} from '../../../src/transforms'

function printDefinitionsWithOverrides(root: StructureGroups): string {
  const { nodes } = generateDefinitions(root)

  const [sources, replacements] = compileOverridesDefines(root)
  const sourcePath = path.resolve(__dirname, 'source.ts')
  const source = printNodeList(nodes)
  sources.set(sourcePath, source)

  const program = createMemoryProgram(sources)
  const sourceFile = program.getSourceFile(sourcePath)
  assert(sourceFile !== undefined)

  const result = ts.transform(sourceFile, [
    createOverrideDefineTransformer(program, replacements),
  ])
  assert.strictEqual(result.transformed.length, 1)

  return printer.printFile(result.transformed[0])
}

test('createOverrideDefineTransformer: applies type renames', () => {
  const root = new Message().initRoot(StructureGroups)
  const group = root._initGroups(1).get(0)
  const structures = group._initStructures(2)

  const thing = structures.get(0)
  thing.name = 'Thing'
  thing.fullyQualifiedName = 'workerd::api::Thing'
  thing.tsOverride = 'RenamedThing'
  function referenceThing(type: Type | Member_Nested) {
    const structureType = type._initStructure()
    structureType.name = 'Thing'
    structureType.fullyQualifiedName = 'workerd::api::Thing'
  }

  // Create type root that references Thing in different ways to test renaming
  const root1 = structures.get(1)
  root1.name = 'Root1'
  root1.fullyQualifiedName = 'workerd::api::Root1'
  root1.tsRoot = true
  // Make sure references to original names in overrides get renamed too
  root1.tsOverride = '{ newProp: Thing; }'
  {
    const members = root1._initMembers(3)

    const prop = members.get(0)._initProperty()
    prop.name = 'prop'
    referenceThing(prop._initType())

    const method = members.get(1)._initMethod()
    method.name = 'method'
    referenceThing(method._initArgs(1).get(0))
    referenceThing(method._initReturnType())

    const nested = members.get(2)._initNested()
    nested.name = 'Thing' // Should keep original name
    referenceThing(nested)
  }

  assert.strictEqual(
    printDefinitionsWithOverrides(root),
    `declare abstract class RenamedThing {
}
interface Root1 {
    prop: RenamedThing;
    method(param0: RenamedThing): RenamedThing;
    Thing: typeof RenamedThing;
    newProp: RenamedThing;
}
`,
  )
})

test('createOverrideDefineTransformer: applies property overrides', () => {
  const root = new Message().initRoot(StructureGroups)
  const group = root._initGroups(1).get(0)
  const structures = group._initStructures(1)

  const root1 = structures.get(0)
  root1.name = 'Root1'
  root1.fullyQualifiedName = 'workerd::api::Root1'
  root1.tsRoot = true
  {
    const members = root1._initMembers(6)

    // Readonly instance property, overridden to be mutable and optional
    const prop1 = members.get(0)._initProperty()
    prop1.name = 'prop1'
    prop1._initType()._initString().name = 'kj::String'
    prop1.readonly = true

    // Mutable instance property, overridden to be readonly and required
    const prop2 = members.get(1)._initProperty()
    prop2.name = 'prop2'
    prop2._initType()._initMaybe()._initValue().boolt = true

    // Readonly prototype property, overridden to be mutable
    const prop3 = members.get(2)._initProperty()
    prop3.name = 'prop3'
    prop3._initType()._initArray()._initElement().boolt = true
    prop3.readonly = true
    prop3.prototype = true

    // Mutable prototype property, overridden to be readonly
    const prop4 = members.get(3)._initProperty()
    prop4.name = 'prop4'
    prop4._initType()._initNumber().name = 'int'
    prop4.prototype = true

    // Deleted property
    const prop5 = members.get(4)._initProperty()
    prop5.name = 'prop5'
    prop5._initType().boolt = true

    // Untouched property
    const prop6 = members.get(5)._initProperty()
    prop6.name = 'prop6'
    prop6._initType()._initPromise()._initValue().voidt = true
  }
  root1.tsOverride = `{
    prop1?: "thing";
    readonly prop2: true;
    get prop3(): false;
    set prop3(value: false);
    get prop4(): 1 | 2 | 3;
    prop5: never;
  }`

  assert.strictEqual(
    printDefinitionsWithOverrides(root),
    `interface Root1 {
    prop1?: "thing";
    readonly prop2: true;
    get prop3(): false;
    set prop3(value: false);
    get prop4(): 1 | 2 | 3;
    prop6: Promise<void>;
}
`,
  )
})

test('createOverrideDefineTransformer: applies method overrides', () => {
  const root = new Message().initRoot(StructureGroups)
  const group = root._initGroups(1).get(0)
  const structures = group._initStructures(1)

  const root1 = structures.get(0)
  root1.name = 'Root1'
  root1.fullyQualifiedName = 'workerd::api::Root1'
  root1.tsRoot = true
  {
    const members = root1._initMembers(7)

    // Static and instance methods with the same names
    const method1 = members.get(0)._initMethod()
    method1.name = 'one'
    method1._initReturnType()._initNumber().name = 'int'
    const staticMethod1 = members.get(1)._initMethod()
    staticMethod1.name = 'one'
    staticMethod1._initReturnType()._initNumber().name = 'int'
    staticMethod1.static = true
    const method2 = members.get(2)._initMethod()
    method2.name = 'two'
    method2._initReturnType()._initNumber().name = 'int'
    const staticMethod2 = members.get(3)._initMethod()
    staticMethod2.name = 'two'
    staticMethod2._initReturnType()._initNumber().name = 'int'
    staticMethod2.static = true

    // Method with multiple overloads
    const methodGet = members.get(4)._initMethod()
    methodGet.name = 'get'
    {
      const args = methodGet._initArgs(2)
      args.get(0)._initString().name = 'kj::String'
      const variants = args.get(1)._initOneOf()._initVariants(2)
      variants.get(0)._initString().name = 'kj::String'
      variants.get(1).unknown = true
    }
    const methodGetReturn = methodGet._initReturnType()._initMaybe()
    methodGetReturn.name = 'kj::Maybe'
    methodGetReturn._initValue().unknown = true

    // Deleted method
    const methodDeleteAll = members.get(5)._initMethod()
    methodDeleteAll.name = 'deleteAll'
    methodDeleteAll._initReturnType().voidt = true

    // Untouched method
    const methodThing = members.get(6)._initMethod()
    methodThing.name = 'thing'
    methodThing._initArgs(1).get(0).boolt = true
    methodThing._initReturnType().boolt = true
  }
  // These overrides test:
  // - Overriding a static method with an instance method of the same name
  // - Overriding an instance method with a static method of the same name
  // - Split overloads, these should be grouped
  // - Deleted method
  root1.tsOverride = `{
    static one(): 1;
    two(): 2;

    get(key: string, type: "text"): Promise<string | null>;
    get(key: string, type: "arrayBuffer"): Promise<ArrayBuffer | null>;

    deleteAll: never;

    get<T>(key: string, type: "json"): Promise<T | null>;
  }`

  assert.strictEqual(
    printDefinitionsWithOverrides(root),
    `declare abstract class Root1 {
    one(): number;
    static one(): 1;
    two(): 2;
    static two(): number;
    get(key: string, type: "text"): Promise<string | null>;
    get(key: string, type: "arrayBuffer"): Promise<ArrayBuffer | null>;
    get<T>(key: string, type: "json"): Promise<T | null>;
    thing(param0: boolean): boolean;
}
`,
  )
})

test('createOverrideDefineTransformer: applies type parameter overrides', () => {
  const root = new Message().initRoot(StructureGroups)
  const group = root._initGroups(1).get(0)
  const structures = group._initStructures(2)

  const struct = structures.get(0)
  struct.name = 'Struct'
  struct.fullyQualifiedName = 'workerd::api::Struct'
  {
    const members = struct._initMembers(1)
    const prop = members.get(0)._initProperty()
    prop.name = 'type'
    prop._initType().unknown = true
  }
  struct.tsOverride = `RenamedStruct<Type extends string = string> {
    type: Type;
  }`

  const root1 = structures.get(1)
  root1.name = 'Root1'
  root1.fullyQualifiedName = 'workerd::api::Root1'
  root1.tsRoot = true
  {
    const members = root1._initMembers(2)

    const methodGet = members.get(0)._initMethod()
    methodGet.name = 'get'
    const returnStruct = methodGet._initReturnType()._initStructure()
    returnStruct.name = 'Struct'
    returnStruct.fullyQualifiedName = 'workerd::api::Struct'

    const methodRead = members.get(1)._initMethod()
    methodRead.name = 'read'
    methodRead._initReturnType()._initPromise()._initValue().unknown = true
  }
  root1.tsOverride = `<R> {
    read(): Promise<R>;
  }`

  assert.strictEqual(
    printDefinitionsWithOverrides(root),
    `interface RenamedStruct<Type extends string = string> {
    type: Type;
}
interface Root1<R> {
    get(): RenamedStruct;
    read(): Promise<R>;
}
`,
  )
})

test('createOverrideDefineTransformer: applies heritage overrides', () => {
  const root = new Message().initRoot(StructureGroups)
  const group = root._initGroups(1).get(0)
  const structures = group._initStructures(4)

  const superclass = structures.get(0)
  superclass.name = `Superclass`
  superclass.fullyQualifiedName = `workerd::api::Superclass`
  superclass.tsOverride = '<T, U = unknown>'

  const root1 = structures.get(1)
  root1.name = 'Root1'
  root1.fullyQualifiedName = 'workerd::api::Root1'
  const root1Extends = root1._initExtends()._initStructure()
  root1Extends.name = 'Superclass'
  root1Extends.fullyQualifiedName = 'workerd::api::Superclass'
  root1.tsRoot = true
  root1.tsOverride = `extends Superclass<ArrayBuffer | ArrayBufferView, Uint8Array>`

  const root2 = structures.get(2)
  root2.name = 'Root2'
  root2.fullyQualifiedName = 'workerd::api::Root2'
  const root2Extends = root1._initExtends()._initStructure()
  root2Extends.name = 'Superclass'
  root2Extends.fullyQualifiedName = 'workerd::api::Superclass'
  root2.tsRoot = true
  root2.tsOverride = 'Root2<T> implements Superclass<T>'

  const root3 = structures.get(3)
  root3.name = 'Root3'
  root3.fullyQualifiedName = 'workerd::api::Root3'
  const root3Extends = root1._initExtends()._initStructure()
  root3Extends.name = 'Superclass'
  root3Extends.fullyQualifiedName = 'workerd::api::Superclass'
  root3.tsRoot = true
  {
    const members = root3._initMembers(1)
    const prop = members.get(0)._initProperty()
    prop.name = 'prop'
    prop._initType()._initNumber().name = 'int'
  }
  root3.tsOverride = `extends Superclass<boolean> {
    prop: 1 | 2 | 3;
  }`

  assert.strictEqual(
    printDefinitionsWithOverrides(root),
    `declare abstract class Superclass<T, U = unknown> {
}
interface Root1 extends Superclass<ArrayBuffer | ArrayBufferView, Uint8Array> {
}
interface Root2<T> implements Superclass<T> {
}
interface Root3 extends Superclass<boolean> {
    prop: 1 | 2 | 3;
}
`,
  )
})

test('createOverrideDefineTransformer: applies full type replacements', () => {
  const root = new Message().initRoot(StructureGroups)
  const group = root._initGroups(1).get(0)
  const structures = group._initStructures(4)

  const root1 = structures.get(0)
  root1.name = 'Root1'
  root1.fullyQualifiedName = 'workerd::api::Root1'
  root1.tsRoot = true
  root1.tsOverride = `const Root1 = {
    new (): { 0: Root2; 1: Root3; };
  }`

  const root2 = structures.get(1)
  root2.name = 'Root2'
  root2.fullyQualifiedName = 'workerd::api::Root2'
  root2.tsRoot = true
  root2.tsOverride = `enum Root2 { ONE, TWO, THREE; }`

  const root3 = structures.get(2)
  root3.name = 'Root3'
  root3.fullyQualifiedName = 'workerd::api::Root3'
  root3.tsRoot = true
  // Check renames still applied with full-type replacements
  root3.tsOverride = `type RenamedRoot3<T = any> = { done: false; value: T; } | { done: true; value: undefined; }`

  const root4 = structures.get(3)
  root4.name = 'Root4'
  root4.fullyQualifiedName = 'workerd::api::Root4'
  root4.tsRoot = true
  root4.tsOverride = `type Root4 = never`

  assert.strictEqual(
    printDefinitionsWithOverrides(root),
    `declare const Root1 = {
    new(): {
        0: Root2;
        1: RenamedRoot3;
    };
};
declare enum Root2 {
    ONE,
    TWO,
    THREE
}
type RenamedRoot3<T = any> = {
    done: false;
    value: T;
} | {
    done: true;
    value: undefined;
};
`,
  )
})

test('createOverrideDefineTransformer: applies overrides with literals', () => {
  const root = new Message().initRoot(StructureGroups)
  const group = root._initGroups(1).get(0)
  const structures = group._initStructures(1)

  const root1 = structures.get(0)
  root1.name = 'Root1'
  root1.fullyQualifiedName = 'workerd::api::Root1'
  root1.tsRoot = true
  root1.tsOverride = `{
    literalString: "hello";
    literalNumber: 42;
    literalArray: [a: "a", b: 2];
    literalObject: { a: "a"; b: 2; };
    literalTemplate: \`\${string}-\${number}\`;
  }`

  assert.strictEqual(
    printDefinitionsWithOverrides(root),
    `interface Root1 {
    literalString: "hello";
    literalNumber: 42;
    literalArray: [
        a: "a",
        b: 2
    ];
    literalObject: {
        a: "a";
        b: 2;
    };
    literalTemplate: \`\${string}-\${number}\`;
}
`,
  )
})

test('createOverrideDefineTransformer: inserts extra defines', () => {
  const root = new Message().initRoot(StructureGroups)
  const group = root._initGroups(1).get(0)
  const structures = group._initStructures(2)

  const root1 = structures.get(0)
  root1.name = 'Root1'
  root1.fullyQualifiedName = 'workerd::api::Root1'
  root1.tsRoot = true

  const root2 = structures.get(1)
  root2.name = 'Root2'
  root2.fullyQualifiedName = 'workerd::api::Root2'
  root2.tsRoot = true
  root2.tsDefine = 'interface Root2Extra<Type> { prop: Type }'
  root2.tsOverride = 'RenamedRoot2'

  // Check defines inserted before structure
  assert.strictEqual(
    printDefinitionsWithOverrides(root),
    `interface Root1 {
}
interface Root2Extra<Type> {
    prop: Type;
}
interface RenamedRoot2 {
}
`,
  )
})
