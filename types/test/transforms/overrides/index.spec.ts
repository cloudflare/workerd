// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "assert";
import { test } from "node:test";
import path from "path";
import {
  Member_Nested,
  StructureGroups,
  Type,
} from "@workerd/jsg/rtti.capnp.js";
import { Message } from "capnp-ts";
import ts from "typescript";
import { generateDefinitions } from "../../../src/generator";
import { printNodeList, printer } from "../../../src/print";
import { createMemoryProgram } from "../../../src/program";
import {
  compileOverridesDefines,
  createOverrideDefineTransformer,
} from "../../../src/transforms";

function printDefinitionsWithOverrides(root: StructureGroups): string {
  const { nodes } = generateDefinitions(root);

  const [sources, replacements] = compileOverridesDefines(root);
  const sourcePath = path.resolve(__dirname, "source.ts");
  const source = printNodeList(nodes);
  sources.set(sourcePath, source);

  const program = createMemoryProgram(sources);
  const sourceFile = program.getSourceFile(sourcePath);
  assert(sourceFile !== undefined);

  const result = ts.transform(sourceFile, [
    createOverrideDefineTransformer(program, replacements),
  ]);
  assert.strictEqual(result.transformed.length, 1);

  return printer.printFile(result.transformed[0]);
}

test("createOverrideDefineTransformer: applies type renames", () => {
  const root = new Message().initRoot(StructureGroups);
  const group = root.initGroups(1).get(0);
  const structures = group.initStructures(2);

  const thing = structures.get(0);
  thing.setName("Thing");
  thing.setFullyQualifiedName("workerd::api::Thing");
  thing.setTsOverride("RenamedThing");
  function referenceThing(type: Type | Member_Nested) {
    const structureType = type.initStructure();
    structureType.setName("Thing");
    structureType.setFullyQualifiedName("workerd::api::Thing");
  }

  // Create type root that references Thing in different ways to test renaming
  const root1 = structures.get(1);
  root1.setName("Root1");
  root1.setFullyQualifiedName("workerd::api::Root1");
  root1.setTsRoot(true);
  // Make sure references to original names in overrides get renamed too
  root1.setTsOverride("{ newProp: Thing; }");
  {
    const members = root1.initMembers(3);

    const prop = members.get(0).initProperty();
    prop.setName("prop");
    referenceThing(prop.initType());

    const method = members.get(1).initMethod();
    method.setName("method");
    referenceThing(method.initArgs(1).get(0));
    referenceThing(method.initReturnType());

    const nested = members.get(2).initNested();
    nested.setName("Thing"); // Should keep original name
    referenceThing(nested);
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
`
  );
});

test("createOverrideDefineTransformer: applies property overrides", () => {
  const root = new Message().initRoot(StructureGroups);
  const group = root.initGroups(1).get(0);
  const structures = group.initStructures(1);

  const root1 = structures.get(0);
  root1.setName("Root1");
  root1.setFullyQualifiedName("workerd::api::Root1");
  root1.setTsRoot(true);
  {
    const members = root1.initMembers(6);

    // Readonly instance property, overridden to be mutable and optional
    const prop1 = members.get(0).initProperty();
    prop1.setName("prop1");
    prop1.initType().initString().setName("kj::String");
    prop1.setReadonly(true);

    // Mutable instance property, overridden to be readonly and required
    const prop2 = members.get(1).initProperty();
    prop2.setName("prop2");
    prop2.initType().initMaybe().initValue().setBoolt();

    // Readonly prototype property, overridden to be mutable
    const prop3 = members.get(2).initProperty();
    prop3.setName("prop3");
    prop3.initType().initArray().initElement().setBoolt();
    prop3.setReadonly(true);
    prop3.setPrototype(true);

    // Mutable prototype property, overridden to be readonly
    const prop4 = members.get(3).initProperty();
    prop4.setName("prop4");
    prop4.initType().initNumber().setName("int");
    prop4.setPrototype(true);

    // Deleted property
    const prop5 = members.get(4).initProperty();
    prop5.setName("prop5");
    prop5.initType().setBoolt();

    // Untouched property
    const prop6 = members.get(5).initProperty();
    prop6.setName("prop6");
    prop6.initType().initPromise().initValue().setVoidt();
  }
  root1.setTsOverride(`{
    prop1?: "thing";
    readonly prop2: true;
    get prop3(): false;
    set prop3(value: false);
    get prop4(): 1 | 2 | 3;
    prop5: never;
  }`);

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
`
  );
});

test("createOverrideDefineTransformer: applies method overrides", () => {
  const root = new Message().initRoot(StructureGroups);
  const group = root.initGroups(1).get(0);
  const structures = group.initStructures(1);

  const root1 = structures.get(0);
  root1.setName("Root1");
  root1.setFullyQualifiedName("workerd::api::Root1");
  root1.setTsRoot(true);
  {
    const members = root1.initMembers(7);

    // Static and instance methods with the same names
    const method1 = members.get(0).initMethod();
    method1.setName("one");
    method1.initReturnType().initNumber().setName("int");
    const staticMethod1 = members.get(1).initMethod();
    staticMethod1.setName("one");
    staticMethod1.initReturnType().initNumber().setName("int");
    staticMethod1.setStatic(true);
    const method2 = members.get(2).initMethod();
    method2.setName("two");
    method2.initReturnType().initNumber().setName("int");
    const staticMethod2 = members.get(3).initMethod();
    staticMethod2.setName("two");
    staticMethod2.initReturnType().initNumber().setName("int");
    staticMethod2.setStatic(true);

    // Method with multiple overloads
    const methodGet = members.get(4).initMethod();
    methodGet.setName("get");
    {
      const args = methodGet.initArgs(2);
      args.get(0).initString().setName("kj::String");
      const variants = args.get(1).initOneOf().initVariants(2);
      variants.get(0).initString().setName("kj::String");
      variants.get(1).setUnknown();
    }
    const methodGetReturn = methodGet.initReturnType().initMaybe();
    methodGetReturn.setName("kj::Maybe");
    methodGetReturn.initValue().setUnknown();

    // Deleted method
    const methodDeleteAll = members.get(5).initMethod();
    methodDeleteAll.setName("deleteAll");
    methodDeleteAll.initReturnType().setVoidt();

    // Untouched method
    const methodThing = members.get(6).initMethod();
    methodThing.setName("thing");
    methodThing.initArgs(1).get(0).setBoolt();
    methodThing.initReturnType().setBoolt();
  }
  // These overrides test:
  // - Overriding a static method with an instance method of the same name
  // - Overriding an instance method with a static method of the same name
  // - Split overloads, these should be grouped
  // - Deleted method
  root1.setTsOverride(`{
    static one(): 1;
    two(): 2;

    get(key: string, type: "text"): Promise<string | null>;
    get(key: string, type: "arrayBuffer"): Promise<ArrayBuffer | null>;

    deleteAll: never;

    get<T>(key: string, type: "json"): Promise<T | null>;
  }`);

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
`
  );
});

test("createOverrideDefineTransformer: applies type parameter overrides", () => {
  const root = new Message().initRoot(StructureGroups);
  const group = root.initGroups(1).get(0);
  const structures = group.initStructures(2);

  const struct = structures.get(0);
  struct.setName("Struct");
  struct.setFullyQualifiedName("workerd::api::Struct");
  {
    const members = struct.initMembers(1);
    const prop = members.get(0).initProperty();
    prop.setName("type");
    prop.initType().setUnknown();
  }
  struct.setTsOverride(`RenamedStruct<Type extends string = string> {
    type: Type;
  }`);

  const root1 = structures.get(1);
  root1.setName("Root1");
  root1.setFullyQualifiedName("workerd::api::Root1");
  root1.setTsRoot(true);
  {
    const members = root1.initMembers(2);

    const methodGet = members.get(0).initMethod();
    methodGet.setName("get");
    const returnStruct = methodGet.initReturnType().initStructure();
    returnStruct.setName("Struct");
    returnStruct.setFullyQualifiedName("workerd::api::Struct");

    const methodRead = members.get(1).initMethod();
    methodRead.setName("read");
    methodRead.initReturnType().initPromise().initValue().setUnknown();
  }
  root1.setTsOverride(`<R> {
    read(): Promise<R>;
  }`);

  assert.strictEqual(
    printDefinitionsWithOverrides(root),
    `interface RenamedStruct<Type extends string = string> {
    type: Type;
}
interface Root1<R> {
    get(): RenamedStruct;
    read(): Promise<R>;
}
`
  );
});

test("createOverrideDefineTransformer: applies heritage overrides", () => {
  const root = new Message().initRoot(StructureGroups);
  const group = root.initGroups(1).get(0);
  const structures = group.initStructures(4);

  const superclass = structures.get(0);
  superclass.setName(`Superclass`);
  superclass.setFullyQualifiedName(`workerd::api::Superclass`);
  superclass.setTsOverride("<T, U = unknown>");

  const root1 = structures.get(1);
  root1.setName("Root1");
  root1.setFullyQualifiedName("workerd::api::Root1");
  const root1Extends = root1.initExtends().initStructure();
  root1Extends.setName("Superclass");
  root1Extends.setFullyQualifiedName("workerd::api::Superclass");
  root1.setTsRoot(true);
  root1.setTsOverride(
    `extends Superclass<ArrayBuffer | ArrayBufferView, Uint8Array>`
  );

  const root2 = structures.get(2);
  root2.setName("Root2");
  root2.setFullyQualifiedName("workerd::api::Root2");
  const root2Extends = root1.initExtends().initStructure();
  root2Extends.setName("Superclass");
  root2Extends.setFullyQualifiedName("workerd::api::Superclass");
  root2.setTsRoot(true);
  root2.setTsOverride("Root2<T> implements Superclass<T>");

  const root3 = structures.get(3);
  root3.setName("Root3");
  root3.setFullyQualifiedName("workerd::api::Root3");
  const root3Extends = root1.initExtends().initStructure();
  root3Extends.setName("Superclass");
  root3Extends.setFullyQualifiedName("workerd::api::Superclass");
  root3.setTsRoot(true);
  {
    const members = root3.initMembers(1);
    const prop = members.get(0).initProperty();
    prop.setName("prop");
    prop.initType().initNumber().setName("int");
  }
  root3.setTsOverride(`extends Superclass<boolean> {
    prop: 1 | 2 | 3;
  }`);

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
`
  );
});

test("createOverrideDefineTransformer: applies full type replacements", () => {
  const root = new Message().initRoot(StructureGroups);
  const group = root.initGroups(1).get(0);
  const structures = group.initStructures(4);

  const root1 = structures.get(0);
  root1.setName("Root1");
  root1.setFullyQualifiedName("workerd::api::Root1");
  root1.setTsRoot(true);
  root1.setTsOverride(`const Root1 = {
    new (): { 0: Root2; 1: Root3; };
  }`);

  const root2 = structures.get(1);
  root2.setName("Root2");
  root2.setFullyQualifiedName("workerd::api::Root2");
  root2.setTsRoot(true);
  root2.setTsOverride(`enum Root2 { ONE, TWO, THREE; }`);

  const root3 = structures.get(2);
  root3.setName("Root3");
  root3.setFullyQualifiedName("workerd::api::Root3");
  root3.setTsRoot(true);
  // Check renames still applied with full-type replacements
  root3.setTsOverride(
    `type RenamedRoot3<T = any> = { done: false; value: T; } | { done: true; value: undefined; }`
  );

  const root4 = structures.get(3);
  root4.setName("Root4");
  root4.setFullyQualifiedName("workerd::api::Root4");
  root4.setTsRoot(true);
  root4.setTsOverride(`type Root4 = never`);

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
`
  );
});

test("createOverrideDefineTransformer: applies overrides with literals", () => {
  const root = new Message().initRoot(StructureGroups);
  const group = root.initGroups(1).get(0);
  const structures = group.initStructures(1);

  const root1 = structures.get(0);
  root1.setName("Root1");
  root1.setFullyQualifiedName("workerd::api::Root1");
  root1.setTsRoot(true);
  root1.setTsOverride(`{
    literalString: "hello";
    literalNumber: 42;
    literalArray: [a: "a", b: 2];
    literalObject: { a: "a"; b: 2; };
    literalTemplate: \`\${string}-\${number}\`;
  }`);

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
`
  );
});

test("createOverrideDefineTransformer: inserts extra defines", () => {
  const root = new Message().initRoot(StructureGroups);
  const group = root.initGroups(1).get(0);
  const structures = group.initStructures(2);

  const root1 = structures.get(0);
  root1.setName("Root1");
  root1.setFullyQualifiedName("workerd::api::Root1");
  root1.setTsRoot(true);

  const root2 = structures.get(1);
  root2.setName("Root2");
  root2.setFullyQualifiedName("workerd::api::Root2");
  root2.setTsRoot(true);
  root2.setTsDefine("interface Root2Extra<Type> { prop: Type }");
  root2.setTsOverride("RenamedRoot2");

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
`
  );
});
