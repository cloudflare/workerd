// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright (c) 2014, StrongLoop Inc.
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

import type { Readable } from 'node:stream'
import type {
  After,
  Before,
  HeapCodeStatistics,
  HeapInfo,
  HeapSnapshotOptions,
  HeapSpaceStatistics,
  HookCallbacks,
  Init,
  Settled,
} from 'node:v8'
import type { Buffer } from 'node-internal:internal_buffer'
import {
  ERR_INVALID_ARG_VALUE,
  ERR_METHOD_NOT_IMPLEMENTED,
} from 'node-internal:internal_errors'
import {
  validateBoolean,
  validateFunction,
  validateObject,
  validateOneOf,
  validateString,
  validateUint32,
} from 'node-internal:validators'

export const cachedDataVersionTag = 0

function getHeapSnapshotOptions(options: HeapSnapshotOptions = {}): void {
  validateObject(options, 'options')
  const { exposeInternals = false, exposeNumericValues = false } = options
  validateBoolean(exposeInternals, 'options.exposeInternals')
  validateBoolean(exposeNumericValues, 'options.exposeNumericValues')
}

// Other than the Serializer/Deserializer classes, most of the API here is not
// something that we intend to implement.

export function writeHeapSnapshot(
  filename?: string,
  options: HeapSnapshotOptions = {},
): void {
  if (filename !== undefined) {
    validateString(filename, 'filename')
  }
  getHeapSnapshotOptions(options)
  throw new ERR_METHOD_NOT_IMPLEMENTED('writeHeapSnapshot')
}

export function getHeapSnapshot(options: HeapSnapshotOptions = {}): Readable {
  getHeapSnapshotOptions(options)
  throw new ERR_METHOD_NOT_IMPLEMENTED('getHeapSnapshot')
}

export function setFlagsFromString(flags: string): void {
  validateString(flags, 'flags')
  throw new ERR_METHOD_NOT_IMPLEMENTED('setFlagsFromString')
}

export function isStringOneByteRepresentation(content: string): boolean {
  // TODO(later): We can implement this later
  validateString(content, 'content')
  throw new ERR_METHOD_NOT_IMPLEMENTED('isStringOneByteRepresentation')
}

export function getHeapStatistics(): HeapInfo {
  return {
    total_heap_size: 0,
    total_heap_size_executable: 0,
    total_physical_size: 0,
    total_available_size: 0,
    used_heap_size: 0,
    heap_size_limit: 0,
    malloced_memory: 0,
    peak_malloced_memory: 0,
    does_zap_garbage: 0,
    number_of_native_contexts: 0,
    number_of_detached_contexts: 0,
    total_global_handles_size: 0,
    used_global_handles_size: 0,
    external_memory: 0,
  }
}

export function getHeapSpaceStatistics(): HeapSpaceStatistics[] {
  return []
}

export function getHeapCodeStatistics(): HeapCodeStatistics {
  return {
    code_and_metadata_size: 0,
    bytecode_and_metadata_size: 0,
    external_script_source_size: 0,
    // @ts-expect-error TS2353 cpu_profiler_metadata_size is not in node/types yet
    cpu_profiler_metadata_size: 0,
  }
}

export function setHeapSnapshotNearHeapLimit(limit: number): void {
  validateUint32(limit, 'limit', true)
  throw new ERR_METHOD_NOT_IMPLEMENTED('setHeapSnapshotNearHeapLimit')
}

export function getCppHeapStatistics(type = 'detailed'): object {
  validateOneOf(type, 'type', ['brief', 'detailed'])
  return {
    committed_size_bytes: 0,
    resident_size_bytes: 0,
    used_size_bytes: 0,
    space_statistics: [],
    type_names: [],
    detail_level: type,
  }
}

// TODO(later): Implement Serializer/Deserializer later
export class Serializer {
  constructor() {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Serializer')
  }

  writeHeader(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('writeHeader')
  }
  writeValue(_value: unknown): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('writeValue')
  }
  releaseBuffer(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('releaseBuffer')
  }
  transferArrayBuffer(_id: number, _arrayBuffer: ArrayBuffer): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('transferArrayBuffer')
  }
  writeUint32(_value: number): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('writeUint32')
  }
  writeUint64(_hi: number, _lo: number): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('writeUint64')
  }
  writeDouble(_value: number): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('writeDouble')
  }
  writeRawBytes(_buffer: NodeJS.TypedArray | Buffer): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('writeRawBytes')
  }
  _getDataCloneError(_message: unknown): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('_getDataCloneError')
  }
  _getSharedArrayBufferId(_sharedArrayBuffer: SharedArrayBuffer): number {
    throw new ERR_METHOD_NOT_IMPLEMENTED('_getSharedArrayBufferId')
  }
  _setTreatArrayBufferViewsAsHostObjects(_flag: boolean): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED(
      '_setTreatArrayBufferViewsAsHostObjects',
    )
  }

  _writeHostObject(_abView: NodeJS.TypedArray | Buffer): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('_writeHostObject')
  }
}

export class Deserializer {
  constructor(_buffer: NodeJS.TypedArray | Buffer) {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Deserializer')
  }
  readHeader(): boolean {
    throw new ERR_METHOD_NOT_IMPLEMENTED('readHeader')
  }

  readValue(): unknown {
    throw new ERR_METHOD_NOT_IMPLEMENTED('readValue')
  }

  transferArrayBuffer(_id: number, _arrayBuffer: ArrayBuffer): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('transferArrayBuffer')
  }

  getWireFormatVersion(): number {
    throw new ERR_METHOD_NOT_IMPLEMENTED('getWireFormatVersion')
  }

  readUint32(): number {
    throw new ERR_METHOD_NOT_IMPLEMENTED('readUint32')
  }

  readUint64(): [number, number] {
    throw new ERR_METHOD_NOT_IMPLEMENTED('readUint64')
  }

  readDouble(): number {
    throw new ERR_METHOD_NOT_IMPLEMENTED('readDouble')
  }
  readRawBytes(_length: number): Buffer {
    throw new ERR_METHOD_NOT_IMPLEMENTED('readRawBytes')
  }

  _readHostObject(): object {
    throw new ERR_METHOD_NOT_IMPLEMENTED('_readHostObject')
  }
}

export class DefaultSerializer extends Serializer {
  constructor() {
    super()
    throw new ERR_METHOD_NOT_IMPLEMENTED('DefaultSerializer')
  }
}

export class DefaultDeserializer extends Deserializer {
  constructor(buffer: NodeJS.TypedArray | Buffer) {
    super(buffer)
    throw new ERR_METHOD_NOT_IMPLEMENTED('DefaultDeserializer')
  }
}

export function serialize(_value: unknown): Buffer {
  // TODO(later): This is one that we can implement later.
  throw new ERR_METHOD_NOT_IMPLEMENTED('serialize')
}

export function deserialize(_buffer: NodeJS.TypedArray | Buffer): unknown {
  // TODO(;later): This is one that we can implement later.
  throw new ERR_METHOD_NOT_IMPLEMENTED('deserialize')
}

export class GCProfiler {
  start(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('GCProfiler.start')
  }

  stop(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('GCProfiler.stop')
  }
}

export const promiseHooks = {
  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  createHook(_: HookCallbacks): Function {
    throw new ERR_METHOD_NOT_IMPLEMENTED('promiseHooks.createHook')
  },

  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  onInit(_: Init): Function {
    throw new ERR_METHOD_NOT_IMPLEMENTED('promiseHooks.onInit')
  },

  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  onBefore(_: Before): Function {
    throw new ERR_METHOD_NOT_IMPLEMENTED('promiseHooks.onBefore')
  },

  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  onAfter(_: After): Function {
    throw new ERR_METHOD_NOT_IMPLEMENTED('promiseHooks.onAfter')
  },

  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  onSettled(_: Settled): Function {
    throw new ERR_METHOD_NOT_IMPLEMENTED('promiseHooks.onSettled')
  },
}

export function queryObjects(
  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  ctor: Function,
  options: { format?: 'count' | 'summary' } = {},
): number | string[] {
  validateFunction(ctor, 'constructor')
  validateObject(options, 'options')
  const format = options.format || 'count'
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (format !== 'count' && format !== 'summary') {
    throw new ERR_INVALID_ARG_VALUE('options.format', format)
  }
  throw new ERR_METHOD_NOT_IMPLEMENTED('queryObjects')
}

export function takeCoverage(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('takeCoverage')
}

export function stopCoverage(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('stopCoverage')
}

export const startupSnapshot = {
  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  addSerializeCallback(_callback: Function, _data: unknown): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('startupSnapshot.addSerializeCallback')
  },

  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  addDeserializeCallback(_callback: Function, _data: unknown): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED(
      'startupSnapshot.addDeserializeCallback',
    )
  },

  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  setDeserializeMainFunction(_callback: Function, _data: unknown): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED(
      'startupSnapshot.setDeserializeMainFunction',
    )
  },
  isBuildingSnapshot(): boolean {
    return false
  },
}

export default {
  cachedDataVersionTag,
  getHeapSnapshot,
  getHeapStatistics,
  getHeapSpaceStatistics,
  getHeapCodeStatistics,
  getCppHeapStatistics,
  setFlagsFromString,
  Serializer,
  Deserializer,
  DefaultSerializer,
  DefaultDeserializer,
  deserialize,
  takeCoverage,
  stopCoverage,
  serialize,
  writeHeapSnapshot,
  promiseHooks,
  queryObjects,
  startupSnapshot,
  setHeapSnapshotNearHeapLimit,
  GCProfiler,
  isStringOneByteRepresentation,
}
