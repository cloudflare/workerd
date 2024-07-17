import assert from "assert";
import fs from "fs/promises";
import { test } from "node:test";
import path from "path";
import { BuiltinType_Type, StructureGroups } from "@workerd/jsg/rtti.capnp.js";
import { Message } from "capnp-ts";
import { main } from "../scripts/build-types";
import { printDefinitions } from "../src";
import { readComments } from "../scripts/build-worker";

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
  eventTarget.setTsDefine("interface Event {}");
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
  const outputPath = path.join(definitionsDir, "types", "index.d.ts");
  const comments = await readComments();

  await fs.writeFile(inputPath, new Uint8Array(message.toArrayBuffer()));
  const { ambient } = await printDefinitions(
    message.getRoot(StructureGroups),
    comments,
    ""
  );

  assert.strictEqual(
    ambient,
    `/*! *****************************************************************************
Copyright (c) Cloudflare. All rights reserved.
Copyright (c) Microsoft Corporation. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License"); you may not use
this file except in compliance with the License. You may obtain a copy of the
License at http://www.apache.org/licenses/LICENSE-2.0
THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
MERCHANTABLITY OR NON-INFRINGEMENT.
See the Apache Version 2.0 License for specific language governing permissions
and limitations under the License.
***************************************************************************** */
/* eslint-disable */
// noinspection JSUnusedGlobalSymbols
/**
 * An event which takes place in the DOM.
 *
 * [MDN Reference](https://developer.mozilla.org/docs/Web/API/Event)
 */
interface Event {
}
/**
 * EventTarget is a DOM interface implemented by objects that can receive events and may have listeners for them.
 *
 * [MDN Reference](https://developer.mozilla.org/docs/Web/API/EventTarget)
 */
declare class EventTarget<EventMap extends Record<string, Event> = Record<string, Event>> {
    constructor();
    /**
     * Appends an event listener for events whose type attribute value is type. The callback argument sets the callback that will be invoked when the event is dispatched.
     *
     * The options argument sets listener-specific options. For compatibility this can be a boolean, in which case the method behaves exactly as if the value was specified as options's capture.
     *
     * When set to true, options's capture prevents callback from being invoked when the event's eventPhase attribute value is BUBBLING_PHASE. When false (or not present), callback will not be invoked when event's eventPhase attribute value is CAPTURING_PHASE. Either way, callback will be invoked if event's eventPhase attribute value is AT_TARGET.
     *
     * When set to true, options's passive indicates that the callback will not cancel the event by invoking preventDefault(). This is used to enable performance optimizations described in § 2.8 Observing event listeners.
     *
     * When set to true, options's once indicates that the callback will only be invoked once after which the event listener will be removed.
     *
     * If an AbortSignal is passed for options's signal, then the event listener will be removed when signal is aborted.
     *
     * The event listener is appended to target's event listener list and is not appended if it has the same type, callback, and capture.
     *
     * [MDN Reference](https://developer.mozilla.org/docs/Web/API/EventTarget/addEventListener)
     */
    addEventListener<Type extends keyof EventMap>(type: Type, handler: (event: EventMap[Type]) => void): void;
}
type WorkerGlobalScopeEventMap = {
    fetch: Event;
    scheduled: Event;
};
declare abstract class WorkerGlobalScope extends EventTarget<WorkerGlobalScopeEventMap> {
}
/**
 * This ServiceWorker API interface represents the global execution context of a service worker.
 * Available only in secure contexts.
 *
 * [MDN Reference](https://developer.mozilla.org/docs/Web/API/ServiceWorkerGlobalScope)
 */
interface ServiceWorkerGlobalScope extends WorkerGlobalScope {
    things(param0: boolean): IterableIterator<string>;
    get prop(): Promise<number>;
}
declare function addEventListener<Type extends keyof WorkerGlobalScopeEventMap>(type: Type, handler: (event: WorkerGlobalScopeEventMap[Type]) => void): void;
declare function things(param0: boolean): IterableIterator<string>;
declare const prop: Promise<number>;
`
  );
});
