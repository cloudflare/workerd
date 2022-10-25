import assert from "assert";
import fs from "fs/promises";
import { test } from "node:test";
import path from "path";
import { StructureGroups } from "@workerd/jsg/rtti.capnp.js";
import { Message } from "capnp-ts";
import { main } from "../src";

test("main: generates types", async () => {
  const message = new Message();
  const root = message.initRoot(StructureGroups);
  const groups = root.initGroups(1);
  const group = groups.get(0);
  group.setName("definitions");
  const structures = group.initStructures(3);

  const root1 = structures.get(0);
  root1.setName("ServiceWorkerGlobalScope");
  root1.setFullyQualifiedName("workerd::api::ServiceWorkerGlobalScope");
  root1.setTsRoot(true);
  {
    const members = root1.initMembers(2);

    // Test that global extraction is performed after iterator processing
    const method = members.get(0).initMethod();
    method.setName("things");
    const methodArgs = method.initArgs(1);
    methodArgs.get(0).setBoolt();
    const methodReturn = method.initReturnType().initStructure();
    methodReturn.setName("ThingIterator");
    methodReturn.setFullyQualifiedName("workerd::api::ThingIterator");

    const prop = members.get(1).initProperty();
    prop.setName("prop");
    prop.setReadonly(true);
    prop.setPrototype(true);
    prop.initType().initPromise().initValue().initNumber().setName("int");
  }

  const iterator = structures.get(1);
  iterator.setName("ThingIterator");
  iterator.setFullyQualifiedName("workerd::api::ThingIterator");
  iterator.initExtends().initIntrinsic().setName("v8::kIteratorPrototype");
  iterator.setIterable(true);
  {
    const members = iterator.initMembers(1);
    const nextMethod = members.get(0).initMethod();
    nextMethod.setName("next");
    const nextStruct = nextMethod.initReturnType().initStructure();
    nextStruct.setName("ThingIteratorNext");
    nextStruct.setFullyQualifiedName("workerd::api::ThingIteratorNext");
    const iteratorMethod = iterator.initIterator();
    iteratorMethod.initReturnType().setUnknown();
  }
  const iteratorNext = structures.get(2);
  iteratorNext.setName("ThingIteratorNext");
  iteratorNext.setFullyQualifiedName("workerd::api::ThingIteratorNext");
  {
    const members = iteratorNext.initMembers(2);
    const doneProp = members.get(0).initProperty();
    doneProp.setName("done");
    doneProp.initType().setBoolt();
    const valueProp = members.get(1).initProperty();
    valueProp.setName("value");
    const valueType = valueProp.initType().initMaybe();
    valueType.setName("jsg::Optional");
    valueType.initValue().initString().setName("kj::String");
  }

  // TODO: add another struct with defines/overrides

  // https://bazel.build/reference/test-encyclopedia#initial-conditions
  const tmpPath = process.env.TEST_TMPDIR;
  assert(tmpPath !== undefined);
  const inputPath = path.join(tmpPath, "types.capnp.bin");
  const outputPath = path.join(tmpPath, "types.d.ts");

  await fs.writeFile(inputPath, new Uint8Array(message.toArrayBuffer()));

  await main([inputPath, "--output", outputPath]);
  let output = await fs.readFile(outputPath, "utf8");
  assert.strictEqual(
    output,
    `/* eslint-disable */
// noinspection JSUnusedGlobalSymbols
export interface ServiceWorkerGlobalScope {
    things(param0: boolean): IterableIterator<string>;
    get prop(): Promise<number>;
}
export declare function things(param0: boolean): IterableIterator<string>;
export declare const prop: Promise<number>;
`
  );

  // Test formatted output
  await main([inputPath, "-o", outputPath, "--format"]);
  output = await fs.readFile(outputPath, "utf8");
  assert.strictEqual(
    output,
    `/* eslint-disable */
// noinspection JSUnusedGlobalSymbols
export interface ServiceWorkerGlobalScope {
  things(param0: boolean): IterableIterator<string>;
  get prop(): Promise<number>;
}
export declare function things(param0: boolean): IterableIterator<string>;
export declare const prop: Promise<number>;
`
  );
});
