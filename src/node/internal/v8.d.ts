// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

type SerializerHandle = unknown;
type DeserializerHandle = unknown;

export declare const SerializerHandle: {
  prototype: SerializerHandle;
  writeHeader(): void;
  writeValue(value: any): boolean;
  writeUint32(value: number): void;
  writeUint64(hi: number, lo: number): void;
  writeDouble(value: number): void;
  writeRawBytes(buffer: ArrayBufferView): void;
  releaseBuffer(): ArrayBuffer;
  transferArrayBuffer(id: number, arrayBuffer: ArrayBuffer): void;
  setTreatArrayBufferViewsAsHostObjects(flag: boolean): void;
}

export declare const DeserializerHandle: {
  prototype: DeserializerHandle;
  readHeader(): boolean;
  readValue(): any;
  readUint32(): number;
  readUint64(): number[];
  readDouble(): number;
  readRawBytes(length: number): number;
  readHostObject(): any;
  transferArrayBuffer(id: number, arrayBuffer: ArrayBuffer): void;
  getWireFormatVersion(): number;
}

export const MIN_SERIALIZATION_VERSION: number;
export const MAX_SERIALIZATION_VERSION: number;
