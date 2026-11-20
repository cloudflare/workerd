'use strict';

// CryptoKey, under the TS_WEBCRYPTO autogate, which replaces the C++ class
// (api/crypto/crypto.h) as the global CryptoKey. The key stays the C++
// CryptoKey: an instance keeps its wrapper under a V8-private key, and C++
// makes every instance, without running this module (see JsCryptoKey in
// api/crypto/crypto.h). See crypto/AGENTS.md.

import type { RuntimeClassHelpers } from '../runtime-class';
import type { SerializationHelpers } from '../serialization';
import type { CommonHelpers } from '../webstreams/common';

const { ObjectCreate, ObjectDefineProperty, ObjectPrototype, TypeError } =
  primordials;

const { markAsRuntimeClass } = require('runtime-class') as RuntimeClassHelpers;
const { markUncloneable } = require('serialization') as SerializationHelpers;
const { kNullProtoObject } = require('webstreams/common') as Pick<
  CommonHelpers,
  'kNullProtoObject'
>;

const kImmutablePrototypes = compatFlags['immutable_api_prototypes'] === true;

// A function, not a class, so that calling it without `new` throws the same
// TypeError as constructing it, and so that its prototype can be writable.
function CryptoKey(): never {
  throw new TypeError('Illegal constructor');
}

// The C++ class's prototype has no members but the constructor.
const proto = ObjectCreate(ObjectPrototype) as object;
ObjectDefineProperty(proto, 'constructor', {
  __proto__: kNullProtoObject,
  value: CryptoKey,
  writable: true,
  enumerable: false,
  configurable: true,
} as PropertyDescriptor);
// Writable, as a JSG class's is, unless immutable_api_prototypes.
ObjectDefineProperty(CryptoKey, 'prototype', {
  __proto__: kNullProtoObject,
  value: proto,
  writable: !kImmutablePrototypes,
} as PropertyDescriptor);
markAsRuntimeClass(proto, 'CryptoKey');

// What C++ copies to make an instance, then gives its values. A copy keeps the
// template's shape, so making a key adds no properties: the wrapper's
// V8-private slot, the uncloneable mark, and the four read-only properties, in
// the C++ class's order, which is the reverse of its JSG_RESOURCE_TYPE's.
const template = ObjectCreate(proto) as Record<PropertyKey, unknown>;
template[utils.getPrivateSymbol('workerd:nativeHandle')] = undefined;
markUncloneable(template);
function defineReadOnly(name: string): void {
  ObjectDefineProperty(template, name, {
    __proto__: kNullProtoObject,
    value: undefined,
    writable: false,
    enumerable: true,
    configurable: true,
  } as PropertyDescriptor);
}
defineReadOnly('usages');
defineReadOnly('algorithm');
defineReadOnly('extractable');
defineReadOnly('type');

export type CryptoKeyExports = {
  CryptoKey: typeof CryptoKey;
  CryptoKeyTemplate: object;
};

module.exports = {
  CryptoKey,
  CryptoKeyTemplate: template,
};
