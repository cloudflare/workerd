'use strict';

// The global TextEncoder, installed by main.ts under the TS_TEXT_ENCODER
// autogate in place of the C++ class (api/encoding.h).
//
// This module is the WebIDL layer: the receiver checks and the argument
// conversions, in the C++ class's order and with its messages. The static
// methods of TextEncoderUtil (src/rust/encoding/text_encoder.rs) encode.
//
// main.ts loads this module on first use, so its top level runs after user
// code and reads nothing that code can change.
//
// DIFFERENCES FROM THE C++ IMPLEMENTATION
//
// - The constructor's `prototype` is non-writable even without
//   immutable_api_prototypes, and `constructor` is the prototype's first
//   property rather than the last of its members. Calling the constructor
//   without `new` throws the language's TypeError rather than JSG's, and
//   util.inspect() shows the constructor as a class.
// - Under workers_api_getters_setters_on_prototype, assigning to `encoding` in
//   strict mode throws V8's TypeError for a property with only a getter,
//   rather than the one for a read-only property.

import type { JsgErrorMessages } from './jsg-errors';
import type { RuntimeClassHelpers } from './runtime-class';
import type { SerializationHelpers } from './serialization';

const {
  ObjectDefineProperties,
  TypeError: TypeErrorCtor,
  TypedArrayCtorByName,
  TypedArrayPrototypeGetSymbolToStringTag,
} = primordials;

const { encode: nativeEncode, encodeInto: nativeEncodeInto } =
  utils.getTextEncoderUtil();
const { kIllegalInvocation } = require('jsg-errors') as JsgErrorMessages;
const { markAsRuntimeClass, setNameAndLength } =
  require('runtime-class') as RuntimeClassHelpers;
const { markUncloneable } = require('serialization') as SerializationHelpers;

// What nativeEncodeInto() writes: the code units read and bytes written. One
// array serves every call.
const kEncodeIntoCounts = new TypedArrayCtorByName.Uint32Array(2);

const kSpecCompliantPropertyAttributes =
  compatFlags['spec_compliant_property_attributes'] === true;

// `encoding` is an own property of each instance unless
// workers_api_getters_setters_on_prototype, as JSG places it.
const kInstanceProperty =
  compatFlags['workers_api_getters_setters_on_prototype'] !== true;
const kInstanceDescriptors = {
  __proto__: null,
  encoding: {
    __proto__: null,
    value: 'utf-8',
    writable: false,
    enumerable: true,
    configurable: true,
  },
} as unknown as PropertyDescriptorMap;

const kSourceNotString =
  "Failed to execute 'encodeInto' on 'TextEncoder': parameter 1 is not of " +
  "type 'String'.";
const kDestinationNotUint8Array =
  "Failed to execute 'encodeInto' on 'TextEncoder': parameter 2 is not of " +
  "type 'Uint8Array'.";

let requireTextEncoder: (receiver: unknown) => void;

class TextEncoder {
  #isTextEncoder = true;

  constructor() {
    markUncloneable(this);
    if (kInstanceProperty) {
      ObjectDefineProperties(this, kInstanceDescriptors);
    }
  }

  static {
    requireTextEncoder = (receiver: unknown): void => {
      if (
        ((typeof receiver !== 'object' || receiver === null) &&
          typeof receiver !== 'function') ||
        !(#isTextEncoder in receiver)
      ) {
        throw new TypeErrorCtor(kIllegalInvocation);
      }
    };
  }

  encode(input?: unknown): Uint8Array {
    requireTextEncoder(this);
    return nativeEncode(input === undefined ? '' : `${input as string}`);
  }

  encodeInto(
    source: unknown,
    destination: unknown
  ): { read: number; written: number } {
    requireTextEncoder(this);
    if (arguments.length === 0) {
      throw new TypeErrorCtor(kSourceNotString);
    }
    const string = `${source as string}`;
    if (TypedArrayPrototypeGetSymbolToStringTag(destination) !== 'Uint8Array') {
      throw new TypeErrorCtor(kDestinationNotUint8Array);
    }
    nativeEncodeInto(string, destination as Uint8Array, kEncodeIntoCounts);
    return {
      read: kEncodeIntoCounts[0] as number,
      written: kEncodeIntoCounts[1] as number,
    };
  }
}

function getEncoding(this: unknown): string {
  requireTextEncoder(this);
  return 'utf-8';
}

// The rest makes the class look as JSG's does: its members are enumerable,
// and functions have JSG's names and lengths, which count arguments only under
// spec_compliant_property_attributes.
setNameAndLength(TextEncoder.prototype.encode, 'encode', 0);
setNameAndLength(
  TextEncoder.prototype.encodeInto,
  'encodeInto',
  kSpecCompliantPropertyAttributes ? 2 : 0
);
setNameAndLength(
  getEncoding,
  kSpecCompliantPropertyAttributes ? 'get encoding' : '',
  0
);
ObjectDefineProperties(TextEncoder.prototype, {
  __proto__: null,
  encode: { __proto__: null, enumerable: true },
  encodeInto: { __proto__: null, enumerable: true },
});
if (!kInstanceProperty) {
  ObjectDefineProperties(TextEncoder.prototype, {
    __proto__: null,
    encoding: {
      __proto__: null,
      get: getEncoding,
      enumerable: true,
      configurable: true,
    },
  });
}
markAsRuntimeClass(TextEncoder.prototype, 'TextEncoder');

module.exports = { TextEncoder };
