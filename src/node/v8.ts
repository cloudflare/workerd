// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Deno and Node.js:
// Copyright 2018-2022 the Deno authors. All rights reserved. MIT license.
//
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.
/* eslint-disable */
import { Buffer } from 'node-internal:internal_buffer';
import { default as v8Module } from 'node-internal:v8';
const {
  SerializerHandle,
  DeserializerHandle
} = v8Module;

import {
  isArrayBuffer,
  isArrayBufferView,
} from 'node-internal:internal_types';

import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE_RANGE,
} from 'node-internal:internal_errors';

declare class DOMException {
  constructor(message: string, name: string);
}

export class Serializer {
  #handle: typeof SerializerHandle;

  constructor() {
    this.#handle = new (SerializerHandle as any)();
    const self: any = this;
    Object.defineProperties(this.#handle, {
      _writeHostObject: {
        enumerable: true,
        configurable: true,
        get() {
          if (typeof self._writeHostObject === 'function') {
            return self._writeHostObject.bind(self);
          }
          return undefined;
        }
      },
      _getSharedArrayBufferId: {
        enumerable: true,
        configurable: true,
        get() {
          if (typeof self._getSharedArrayBufferId === 'function') {
            return self._getSharedArrayBufferId.bind(this);
          }
          return undefined;
        }
      }
    });
  }

  writeHeader(): void {
    this.#handle.writeHeader();
  }

  writeValue(value: any): boolean {
    return this.#handle.writeValue(value);
  }

  releaseBuffer(): Buffer {
    return Buffer.from(this.#handle.releaseBuffer());
  }

  transferArrayBuffer(id: number, arrayBuffer: ArrayBuffer): void {
    if (!isArrayBuffer(arrayBuffer)) {
      throw new ERR_INVALID_ARG_TYPE('arrayBuffer', 'ArrayBuffer', arrayBuffer);
    }
    return this.#handle.transferArrayBuffer(id, arrayBuffer);
  }

  writeUint32(value: number): void {
    this.#handle.writeUint32(value);
  }

  writeUint64(hi: number, lo: number): void {
    this.#handle.writeUint64(hi, lo);
  }

  writeDouble(value: number): void {
    this.#handle.writeDouble(value);
  }

  writeRawBytes(buffer: ArrayBufferView): void {
    if (!isArrayBufferView(buffer)) {
      throw new ERR_INVALID_ARG_TYPE('buffer', ['Buffer', 'TypeArray', 'DataView'], buffer);
    }
    this.#handle.writeRawBytes(buffer);
  }

  _getDataCloneError(message: string): DOMException {
    return new DOMException(`${message}`, 'DataCloneError');
  }

  _setTreatArrayBufferViewsAsHostObjects(flag: boolean): void {
    this.#handle.setTreatArrayBufferViewsAsHostObjects(!!flag);
  }
}

export class Deserializer {
  #handle: typeof DeserializerHandle;
  buffer: Buffer;

  constructor(buffer: ArrayBufferView) {
    if (!isArrayBufferView(buffer)) {
      throw new ERR_INVALID_ARG_TYPE('buffer', ['Buffer', 'TypeArray', 'DataView'], buffer);
    }
    this.#handle = new (DeserializerHandle as any)(buffer);
    const self: any = this;
    Object.defineProperties(this.#handle, {
      _readHostObject: {
        enumerable: true,
        configurable: true,
        get() {
          if (typeof self._readHostObject === 'function') {
            return self._readHostObject.bind(self);
          }
          return undefined;
        }
      },
    });
    this.buffer = Buffer.from(buffer.buffer, buffer.byteOffset, buffer.byteLength);
  }

  readHeader(): boolean {
    return this.#handle.readHeader();
  }

  readValue(): any {
    return this.#handle.readValue();
  }

  transferArrayBuffer(id: number, arrayBuffer: ArrayBuffer): void {
    if (!isArrayBuffer(arrayBuffer)) {
      throw new ERR_INVALID_ARG_TYPE('arrayBuffer', 'ArrayBuffer', arrayBuffer);
    }
    return this.#handle.transferArrayBuffer(id, arrayBuffer);
  }

  getWireFormatVersion(): number {
    return this.#handle.getWireFormatVersion();
  }

  readUint32(): number {
    return this.#handle.readUint32();
  }

  readUint64(): number[] {
    return this.#handle.readUint64();
  }

  readDouble(): number {
    return this.#handle.readDouble();
  }

  _readRawBytes(length: number): number {
    return this.#handle.readRawBytes(length);
  }

  readRawBytes(length: number): Buffer {
    if (length < 0) {
      throw new ERR_INVALID_ARG_VALUE_RANGE('length', length, '>= 0');
    }
    const offset = this.#handle.readRawBytes(length);
    return Buffer.from(this.buffer.buffer,
                       this.buffer.byteOffset + offset,
                       length);
  }
}

function arrayBufferViewTypeToIndex(abView: unknown) {
  const type = Object.prototype.toString.call(abView);
  if (abView instanceof Buffer) return 10;
  if (type === '[object Int8Array]') return 0;
  if (type === '[object Uint8Array]') return 1;
  if (type === '[object Uint8ClampedArray]') return 2;
  if (type === '[object Int16Array]') return 3;
  if (type === '[object Uint16Array]') return 4;
  if (type === '[object Int32Array]') return 5;
  if (type === '[object Uint32Array]') return 6;
  if (type === '[object Float32Array]') return 7;
  if (type === '[object Float64Array]') return 8;
  if (type === '[object DataView]') return 9;
  // Index 10 is Buffer.
  if (type === '[object BigInt64Array]') return 11;
  if (type === '[object BigUint64Array]') return 12;
  return -1;
}

function arrayBufferViewIndexToType(index: number) {
  if (index === 0) return Int8Array;
  if (index === 1) return Uint8Array;
  if (index === 2) return Uint8ClampedArray;
  if (index === 3) return Int16Array;
  if (index === 4) return Uint16Array;
  if (index === 5) return Int32Array;
  if (index === 6) return Uint32Array;
  if (index === 7) return Float32Array;
  if (index === 8) return Float64Array;
  if (index === 9) return DataView;
  if (index === 10) return Buffer;
  if (index === 11) return BigInt64Array;
  if (index === 12) return BigUint64Array;
  return undefined;
}

export class DefaultSerializer extends Serializer {
  constructor() {
    super();
    this._setTreatArrayBufferViewsAsHostObjects(true);
  }

  _writeHostObject(abView: any) {
    // Keep track of how to handle different ArrayBufferViews. The default
    // Serializer for Node does not use the V8 methods for serializing those
    // objects because Node's `Buffer` objects use pooled allocation in many
    // cases, and their underlying `ArrayBuffer`s would show up in the
    // serialization. Because a) those may contain sensitive data and the user
    // may not be aware of that and b) they are often much larger than the
    // `Buffer` itself, custom serialization is applied.
    let i = 10;  // Buffer
    if (abView.constructor !== Buffer) {
      i = arrayBufferViewTypeToIndex(abView);
      if (i === -1) {
        throw this._getDataCloneError(`Unserializable host object: ${abView}`);
      }
    }
    this.writeUint32(i);
    this.writeUint32(abView.byteLength);
    this.writeRawBytes(new Uint8Array(abView.buffer,
                                      abView.byteOffset,
                                      abView.byteLength));
    return true;
  }
}

export class DefaultDeserializer extends Deserializer {
  constructor(buffer: ArrayBufferView) {
    super(buffer);
  }

  _readHostObject() : object {
    const typeIndex = this.readUint32();
    const ctor = arrayBufferViewIndexToType(typeIndex);
    if (ctor === undefined) {
      throw new DOMException(`Unsupported host object type index: ${typeIndex}`, 'DataCloneError');
    }
    const byteLength = this.readUint32();
    const byteOffset = this._readRawBytes(byteLength);
    const BYTES_PER_ELEMENT = (ctor as any).BYTES_PER_ELEMENT || 1;

    const offset = this.buffer.byteOffset + byteOffset;
    if (offset % BYTES_PER_ELEMENT === 0) {
      return new (ctor as any)(this.buffer.buffer, offset, byteLength / BYTES_PER_ELEMENT);
    }
    // Copy to an aligned buffer first.
    const buffer_copy = Buffer.concat([this.buffer.slice(byteOffset, byteOffset + byteLength)]);
    return new (ctor as any)(buffer_copy.buffer,
                    buffer_copy.byteOffset,
                    byteLength / BYTES_PER_ELEMENT);
  }
}

export function serialize(value: any): Buffer {
  const serializer = new DefaultSerializer();
  serializer.writeHeader();
  serializer.writeValue(value);
  return serializer.releaseBuffer();
}

export function deserialize(buffer: ArrayBufferView): any {
  const deserializer = new DefaultDeserializer(buffer);
  deserializer.readHeader();
  return deserializer.readValue();
}

export default {
  serialize,
  deserialize,
  Serializer,
  Deserializer,
  DefaultSerializer,
  DefaultDeserializer,
};
