// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert'
import { test } from 'node:test'
import { BuiltinType_Type, JsgImplType_Type, Type } from '@workerd/jsg/rtti'
import { Message } from 'capnp-es'
import { createTypeNode } from '../../src/generator/type'
import { printNode } from '../../src/print'

test('createTypeNode: primitive types', () => {
  const type = new Message().initRoot(Type)

  type.unknown = true
  assert.strictEqual(printNode(createTypeNode(type)), 'any')
  type.object = true
  assert.strictEqual(printNode(createTypeNode(type)), 'any')

  type.voidt = true
  assert.strictEqual(printNode(createTypeNode(type)), 'void')

  type.boolt = true
  assert.strictEqual(printNode(createTypeNode(type)), 'boolean')

  type._initNumber().name = 'int'
  assert.strictEqual(printNode(createTypeNode(type)), 'number')
  type.number.name = 'long'
  assert.strictEqual(printNode(createTypeNode(type)), 'number | bigint')

  type._initString().name = 'kj::String'
  assert.strictEqual(printNode(createTypeNode(type)), 'string')

  const structure = type._initStructure()
  structure.name = 'KvNamespace'
  structure.fullyQualifiedName = 'workerd::api::KvNamespace'
  assert.strictEqual(printNode(createTypeNode(type)), 'KvNamespace')
})

test('createTypeNode: builtin types', () => {
  const type = new Message().initRoot(Type)
  const builtin = type._initBuiltin()

  builtin.type = BuiltinType_Type.V8UINT8ARRAY
  assert.strictEqual(printNode(createTypeNode(type)), 'Uint8Array')

  builtin.type = BuiltinType_Type.V8ARRAY_BUFFER_VIEW
  assert.strictEqual(printNode(createTypeNode(type)), 'ArrayBufferView')

  builtin.type = BuiltinType_Type.JSG_BUFFER_SOURCE
  assert.strictEqual(
    printNode(createTypeNode(type)),
    'ArrayBuffer | ArrayBufferView',
  )

  builtin.type = BuiltinType_Type.KJ_DATE
  assert.strictEqual(printNode(createTypeNode(type)), 'Date')

  builtin.type = BuiltinType_Type.V8FUNCTION
  assert.strictEqual(printNode(createTypeNode(type)), 'Function')

  const intrinsic = type._initIntrinsic()

  intrinsic.name = 'v8::kErrorPrototype'
  assert.strictEqual(printNode(createTypeNode(type)), 'Error')

  intrinsic.name = 'v8::kIteratorPrototype'
  assert.strictEqual(printNode(createTypeNode(type)), 'Iterator<unknown>')

  intrinsic.name = 'v8::kAsyncIteratorPrototype'
  assert.strictEqual(printNode(createTypeNode(type)), 'AsyncIterator<unknown>')
})

test('createTypeNode: generic types', () => {
  const type = new Message().initRoot(Type)

  type._initPromise()._initValue().voidt = true
  assert.strictEqual(printNode(createTypeNode(type)), 'Promise<void>')
  type._initPromise()._initValue().voidt = true
  assert.strictEqual(
    printNode(createTypeNode(type, true)),
    'void | Promise<void>',
  )
  assert.strictEqual(
    printNode(createTypeNode(type, true, true)),
    'Promise<any>',
  )

  const maybe = type._initMaybe()
  maybe._initValue().boolt = true
  maybe.name = 'jsg::Optional'
  assert.strictEqual(printNode(createTypeNode(type)), 'boolean | undefined')
  maybe.name = 'jsg::LenientOptional'
  assert.strictEqual(printNode(createTypeNode(type)), 'boolean | undefined')
  maybe.name = 'kj::Maybe'
  assert.strictEqual(printNode(createTypeNode(type)), 'boolean | null')

  const dict = type._initDict()
  dict._initKey()._initString().name = 'kj::StringPtr'
  dict._initValue()._initNumber().name = 'short'
  assert.strictEqual(printNode(createTypeNode(type)), 'Record<string, number>')

  const variants = type._initOneOf()._initVariants(3)
  variants.get(0).voidt = true
  variants.get(1)._initNumber().name = 'unsigned short'
  variants.get(2)._initString().name = 'kj::String'
  assert.strictEqual(printNode(createTypeNode(type)), 'void | number | string')
})

test('createTypeNode: array types', () => {
  const type = new Message().initRoot(Type)
  const array = type._initArray()

  // Regular array
  array.name = 'kj::Array'
  array._initElement()._initString().name = 'kj::String'
  assert.strictEqual(printNode(createTypeNode(type)), 'string[]')
  assert.strictEqual(printNode(createTypeNode(type, true)), 'string[]')
  // Iterable
  array.name = 'jsg::Sequence'
  array._initElement()._initString().name = 'kj::String'
  assert.strictEqual(printNode(createTypeNode(type)), 'string[]')
  assert.strictEqual(printNode(createTypeNode(type, true)), 'Iterable<string>')

  // Numeric arrays
  array.name = 'kj::Array'
  array._initElement()._initNumber().name = 'int'
  assert.strictEqual(printNode(createTypeNode(type)), 'number[]')
  // If element is a char, then this is a string
  array.name = 'kj::ArrayPtr'
  array._initElement()._initNumber().name = 'char'
  assert.strictEqual(printNode(createTypeNode(type)), 'string')
  // If element is a byte, then this is an ArrayBuffer, ArrayBufferView or both
  array.name = 'kj::Array'
  array._initElement()._initNumber().name = 'unsigned char'
  assert.strictEqual(printNode(createTypeNode(type)), 'ArrayBuffer')
  assert.strictEqual(
    printNode(createTypeNode(type, true)),
    'ArrayBuffer | ArrayBufferView',
  )
  array.name = 'kj::ArrayPtr'
  array._initElement()._initNumber().name = 'unsigned char'
  assert.strictEqual(printNode(createTypeNode(type)), 'ArrayBufferView')
  assert.strictEqual(
    printNode(createTypeNode(type, true)),
    'ArrayBuffer | ArrayBufferView',
  )
})

test('createTypeNode: function types', () => {
  const message = new Message()

  // (a: boolean, b: number | undefined, d: string, c?: any) => void
  let type = message.initRoot(Type)
  let func = type._initFunction()
  let args = func._initArgs(5)
  args.get(0).boolt = true
  args.get(1)._initMaybe()._initValue()._initNumber().name = 'int'
  args.get(2)._initString().name = 'kj::String'
  args.get(3)._initMaybe()._initValue().object = true
  args.get(4)._initJsgImpl().type = JsgImplType_Type.V8ISOLATE
  func._initReturnType().voidt = true
  let typeNode = createTypeNode(type)
  assert.strictEqual(
    printNode(typeNode),
    '(param0: boolean, param1: number | undefined, param2: string, param3?: any) => void',
  )

  // (a?: string, ...b: any[]) => Promise<void>
  type = message.initRoot(Type)
  func = type._initFunction()
  args = func._initArgs(3)
  args.get(0)._initJsgImpl().type = JsgImplType_Type.JSG_TYPE_HANDLER
  args.get(1)._initMaybe()._initValue()._initString().name = 'kj::String'
  args.get(2)._initJsgImpl().type = JsgImplType_Type.JSG_VARARGS
  func._initReturnType()._initPromise()._initValue().voidt = true
  typeNode = createTypeNode(type)
  assert.strictEqual(
    printNode(typeNode),
    '(param1?: string, ...param2: any[]) => void | Promise<void>',
  )
})

test('createTypeNode: implementation types', () => {
  const type = new Message().initRoot(Type)
  const impl = type._initJsgImpl()

  const implTypes: JsgImplType_Type[] = Object.values(type).filter(
    (member) => typeof member === 'number',
  ) as JsgImplType_Type[]
  for (const implType of implTypes) {
    // VARARGS and NAME are the only types we care about which will be tested
    // with function types, the rest should be ignored
    if (
      implType === JsgImplType_Type.JSG_VARARGS ||
      implType === JsgImplType_Type.JSG_NAME
    ) {
      continue
    }
    impl.type = implType
    assert.strictEqual(printNode(createTypeNode(type)), 'never')
  }

  impl.type = JsgImplType_Type.JSG_NAME
  assert.strictEqual(printNode(createTypeNode(type)), 'PropertyKey')
})
