import assert from 'node:assert'
import fs from 'node:fs/promises'
import path from 'node:path'
import { test } from 'node:test'
import { BuiltinType_Type, StructureGroups } from '@workerd/jsg/rtti'
import { Message } from 'capnp-es'
import { readComments } from '../scripts/build-worker'
import { printDefinitions } from '../src'

test('main: generates types', async () => {
  const message = new Message()
  const root = message.initRoot(StructureGroups)
  const groups = root._initGroups(1)
  const group = groups.get(0)
  group.name = 'definitions'
  const structures = group._initStructures(5)

  const eventTarget = structures.get(0)
  eventTarget.name = 'EventTarget'
  eventTarget.fullyQualifiedName = 'workerd::api::EventTarget'
  {
    const members = eventTarget._initMembers(2)
    members.get(0)._initConstructor()
    const method = members.get(1)._initMethod()
    method.name = 'addEventListener'
    {
      const args = method._initArgs(2)
      args.get(0)._initString().name = 'kj::String'
      args.get(1)._initBuiltin().type = BuiltinType_Type.V8FUNCTION
    }
    method._initReturnType().voidt = true
  }
  eventTarget.tsDefine = 'interface Event {}'
  eventTarget.tsOverride = `<EventMap extends Record<string, Event> = Record<string, Event>> {
    addEventListener<Type extends keyof EventMap>(type: Type, handler: (event: EventMap[Type]) => void): void;
  }`

  const workerGlobalScope = structures.get(1)
  workerGlobalScope.name = 'WorkerGlobalScope'
  workerGlobalScope.fullyQualifiedName = 'workerd::api::WorkerGlobalScope'
  let extendsStructure = workerGlobalScope._initExtends()._initStructure()
  extendsStructure.name = 'EventTarget'
  extendsStructure.fullyQualifiedName = 'workerd::api::EventTarget'
  workerGlobalScope.tsDefine = `type WorkerGlobalScopeEventMap = {
    fetch: Event;
    scheduled: Event;
  }`
  workerGlobalScope.tsOverride =
    'extends EventTarget<WorkerGlobalScopeEventMap>'

  const serviceWorkerGlobalScope = structures.get(2)
  serviceWorkerGlobalScope.name = 'ServiceWorkerGlobalScope'
  serviceWorkerGlobalScope.fullyQualifiedName =
    'workerd::api::ServiceWorkerGlobalScope'
  extendsStructure = serviceWorkerGlobalScope._initExtends()._initStructure()
  extendsStructure.name = 'WorkerGlobalScope'
  extendsStructure.fullyQualifiedName = 'workerd::api::WorkerGlobalScope'
  serviceWorkerGlobalScope.tsRoot = true
  {
    const members = serviceWorkerGlobalScope._initMembers(2)

    // Test that global extraction is performed after iterator processing
    const method = members.get(0)._initMethod()
    method.name = 'things'
    const methodArgs = method._initArgs(1)
    methodArgs.get(0).boolt = true
    const methodReturn = method._initReturnType()._initStructure()
    methodReturn.name = 'ThingIterator'
    methodReturn.fullyQualifiedName = 'workerd::api::ThingIterator'

    const prop = members.get(1)._initProperty()
    prop.name = 'prop'
    prop.readonly = true
    prop.prototype = true
    prop._initType()._initPromise()._initValue()._initNumber().name = 'int'
  }

  const iterator = structures.get(3)
  iterator.name = 'ThingIterator'
  iterator.fullyQualifiedName = 'workerd::api::ThingIterator'
  iterator._initExtends()._initIntrinsic().name = 'v8::kIteratorPrototype'
  iterator.iterable = true
  {
    const members = iterator._initMembers(1)
    const nextMethod = members.get(0)._initMethod()
    nextMethod.name = 'next'
    const nextStruct = nextMethod._initReturnType()._initStructure()
    nextStruct.name = 'ThingIteratorNext'
    nextStruct.fullyQualifiedName = 'workerd::api::ThingIteratorNext'
    const iteratorMethod = iterator._initIterator()
    iteratorMethod._initReturnType().unknown = true
  }
  const iteratorNext = structures.get(4)
  iteratorNext.name = 'ThingIteratorNext'
  iteratorNext.fullyQualifiedName = 'workerd::api::ThingIteratorNext'
  {
    const members = iteratorNext._initMembers(2)
    const doneProp = members.get(0)._initProperty()
    doneProp.name = 'done'
    doneProp._initType().boolt = true
    const valueProp = members.get(1)._initProperty()
    valueProp.name = 'value'
    const valueType = valueProp._initType()._initMaybe()
    valueType.name = 'jsg::Optional'
    valueType._initValue()._initString().name = 'kj::String'
  }

  // https://bazel.build/reference/test-encyclopedia#initial-conditions
  const tmpPath = process.env.TEST_TMPDIR
  assert(tmpPath !== undefined)
  const definitionsDir = path.join(tmpPath, 'definitions')
  await fs.mkdir(definitionsDir)
  const inputDir = path.join(tmpPath, 'capnp')
  await fs.mkdir(inputDir)
  const inputPath = path.join(inputDir, 'types.api.capnp.bin')
  const comments = readComments()

  await fs.writeFile(inputPath, new Uint8Array(message.toArrayBuffer()))
  const { ambient } = printDefinitions(
    message.getRoot(StructureGroups),
    comments,
    '',
  )

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
declare var onmessage: never;
/**
 * The **\`Event\`** interface represents an event which takes place on an \`EventTarget\`.
 *
 * [MDN Reference](https://developer.mozilla.org/docs/Web/API/Event)
 */
interface Event {
}
/**
 * The **\`EventTarget\`** interface is implemented by objects that can receive events and may have listeners for them.
 *
 * [MDN Reference](https://developer.mozilla.org/docs/Web/API/EventTarget)
 */
declare class EventTarget<EventMap extends Record<string, Event> = Record<string, Event>> {
    constructor();
    /**
     * The **\`addEventListener()\`** method of the EventTarget interface sets up a function that will be called whenever the specified event is delivered to the target.
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
 * The **\`ServiceWorkerGlobalScope\`** interface of the Service Worker API represents the global execution context of a service worker.
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
`,
  )
})
