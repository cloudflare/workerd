import assert from "assert";
import fs from "fs/promises";
import { test } from "node:test";
import path from "path";
import { BuiltinType_Type, StructureGroups } from "@workerd/jsg/rtti.capnp.js";
import { Message } from "capnp-ts";
import { main } from "../src";

test("main: generates types", async () => {
  const message = new Message();
  const root = message.initRoot(StructureGroups);
  const groups = root.initGroups(1);
  const group = groups.get(0);
  group.setName("definitions");
  const structures = group.initStructures(5);

  const eventTarget = structures.get(0);
  eventTarget.setName("EventTarget");
  eventTarget.setFullyQualifiedName("workerd::api::EventTarget");
  {
    const members = eventTarget.initMembers(2);
    members.get(0).initConstructor();
    const method = members.get(1).initMethod();
    method.setName("addEventListener");
    {
      const args = method.initArgs(2);
      args.get(0).initString().setName("kj::String");
      args.get(1).initBuiltin().setType(BuiltinType_Type.V8FUNCTION);
    }
    method.initReturnType().setVoidt();
  }
  eventTarget.setTsOverride(`<EventMap extends Record<string, Event> = Record<string, Event>> {
    addEventListener<Type extends keyof EventMap>(type: Type, handler: (event: EventMap[Type]) => void): void;
  }`);

  const workerGlobalScope = structures.get(1);
  workerGlobalScope.setName("WorkerGlobalScope");
  workerGlobalScope.setFullyQualifiedName("workerd::api::WorkerGlobalScope");
  let extendsStructure = workerGlobalScope.initExtends().initStructure();
  extendsStructure.setName("EventTarget");
  extendsStructure.setFullyQualifiedName("workerd::api::EventTarget");
  workerGlobalScope.setTsDefine(`type WorkerGlobalScopeEventMap = {
    fetch: Event;
    scheduled: Event;
  }`);
  workerGlobalScope.setTsOverride(
    "extends EventTarget<WorkerGlobalScopeEventMap>"
  );

  const serviceWorkerGlobalScope = structures.get(2);
  serviceWorkerGlobalScope.setName("ServiceWorkerGlobalScope");
  serviceWorkerGlobalScope.setFullyQualifiedName(
    "workerd::api::ServiceWorkerGlobalScope"
  );
  extendsStructure = serviceWorkerGlobalScope.initExtends().initStructure();
  extendsStructure.setName("WorkerGlobalScope");
  extendsStructure.setFullyQualifiedName("workerd::api::WorkerGlobalScope");
  serviceWorkerGlobalScope.setTsRoot(true);
  {
    const members = serviceWorkerGlobalScope.initMembers(2);

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

  const iterator = structures.get(3);
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
  const iteratorNext = structures.get(4);
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

  // https://bazel.build/reference/test-encyclopedia#initial-conditions
  const tmpPath = process.env.TEST_TMPDIR;
  assert(tmpPath !== undefined);
  const definitionsDir = path.join(tmpPath, "definitions");
  await fs.mkdir(definitionsDir);
  const inputDir = path.join(tmpPath, "capnp");
  await fs.mkdir(inputDir);
  const inputPath = path.join(inputDir, "types.api.capnp.bin");
  const outputPath = path.join(definitionsDir, "types", "api.d.ts");

  await fs.writeFile(inputPath, new Uint8Array(message.toArrayBuffer()));

  await main([inputDir, "--output", definitionsDir]);
  let output = await fs.readFile(outputPath, "utf8");
  assert.strictEqual(
    output,
    `/* eslint-disable */
// noinspection JSUnusedGlobalSymbols
declare class EventTarget<EventMap extends Record<string, Event> = Record<string, Event>> {
    constructor();
    addEventListener<Type extends keyof EventMap>(type: Type, handler: (event: EventMap[Type]) => void): void;
}
declare type WorkerGlobalScopeEventMap = {
    fetch: Event;
    scheduled: Event;
};
declare abstract class WorkerGlobalScope extends EventTarget<WorkerGlobalScopeEventMap> {
}
/** This ServiceWorker API interface represents the global execution context of a service worker. */
declare interface ServiceWorkerGlobalScope extends WorkerGlobalScope {
    things(param0: boolean): IterableIterator<string>;
    get prop(): Promise<number>;
}
declare function addEventListener<Type extends keyof WorkerGlobalScopeEventMap>(type: Type, handler: (event: WorkerGlobalScopeEventMap[Type]) => void): void;
declare function things(param0: boolean): IterableIterator<string>;
declare const prop: Promise<number>;
`
  );

  // Test formatted output
  await main([inputDir, "-o", definitionsDir, "--format"]);
  output = await fs.readFile(outputPath, "utf8");
  assert.strictEqual(
    output,
    `/* eslint-disable */
// noinspection JSUnusedGlobalSymbols
declare class EventTarget<
  EventMap extends Record<string, Event> = Record<string, Event>
> {
  constructor();
  addEventListener<Type extends keyof EventMap>(
    type: Type,
    handler: (event: EventMap[Type]) => void
  ): void;
}
declare type WorkerGlobalScopeEventMap = {
  fetch: Event;
  scheduled: Event;
};
declare abstract class WorkerGlobalScope extends EventTarget<WorkerGlobalScopeEventMap> {}
/** This ServiceWorker API interface represents the global execution context of a service worker. */
declare interface ServiceWorkerGlobalScope extends WorkerGlobalScope {
  things(param0: boolean): IterableIterator<string>;
  get prop(): Promise<number>;
}
declare function addEventListener<Type extends keyof WorkerGlobalScopeEventMap>(
  type: Type,
  handler: (event: WorkerGlobalScopeEventMap[Type]) => void
): void;
declare function things(param0: boolean): IterableIterator<string>;
declare const prop: Promise<number>;
`
  );
});
