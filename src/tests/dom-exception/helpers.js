// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared by more than one behavior module.

import { deepStrictEqual, ok, strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// A property descriptor's attributes as a short string: its kind ('data'
// or 'accessor'), then w, e and c for writable, enumerable and
// configurable.
export function attributes(descriptor) {
  return [
    'value' in descriptor ? 'data' : 'accessor',
    descriptor.writable ? 'w' : '',
    descriptor.enumerable ? 'e' : '',
    descriptor.configurable ? 'c' : '',
  ].join('');
}

export function attributesOf(object, key) {
  const descriptor = Object.getOwnPropertyDescriptor(object, key);
  return descriptor === undefined ? 'none' : attributes(descriptor);
}

// The legacy code constants, in the order WebIDL declares them.
export const kLegacyCodes = [
  ['INDEX_SIZE_ERR', 1, 'IndexSizeError'],
  ['DOMSTRING_SIZE_ERR', 2, 'DOMStringSizeError'],
  ['HIERARCHY_REQUEST_ERR', 3, 'HierarchyRequestError'],
  ['WRONG_DOCUMENT_ERR', 4, 'WrongDocumentError'],
  ['INVALID_CHARACTER_ERR', 5, 'InvalidCharacterError'],
  ['NO_DATA_ALLOWED_ERR', 6, 'NoDataAllowedError'],
  ['NO_MODIFICATION_ALLOWED_ERR', 7, 'NoModificationAllowedError'],
  ['NOT_FOUND_ERR', 8, 'NotFoundError'],
  ['NOT_SUPPORTED_ERR', 9, 'NotSupportedError'],
  ['INUSE_ATTRIBUTE_ERR', 10, 'InUseAttributeError'],
  ['INVALID_STATE_ERR', 11, 'InvalidStateError'],
  ['SYNTAX_ERR', 12, 'SyntaxError'],
  ['INVALID_MODIFICATION_ERR', 13, 'InvalidModificationError'],
  ['NAMESPACE_ERR', 14, 'NamespaceError'],
  ['INVALID_ACCESS_ERR', 15, 'InvalidAccessError'],
  ['VALIDATION_ERR', 16, 'ValidationError'],
  ['TYPE_MISMATCH_ERR', 17, 'TypeMismatchError'],
  ['SECURITY_ERR', 18, 'SecurityError'],
  ['NETWORK_ERR', 19, 'NetworkError'],
  ['ABORT_ERR', 20, 'AbortError'],
  ['URL_MISMATCH_ERR', 21, 'URLMismatchError'],
  ['QUOTA_EXCEEDED_ERR', 22, 'QuotaExceededError'],
  ['TIMEOUT_ERR', 23, 'TimeoutError'],
  ['INVALID_NODE_TYPE_ERR', 24, 'InvalidNodeTypeError'],
  ['DATA_CLONE_ERR', 25, 'DataCloneError'],
];

// The own keys of an instance: the C++ gives it name, message and code as
// own properties (ledger #1); both give it a stack (ledger #2). `extra`
// are keys something else added, such as `retryable`.
export function instanceKeys(...extra) {
  const base = usingTsImpl ? ['stack'] : ['code', 'name', 'message', 'stack'];
  return [...base, ...extra];
}

// Asserts `error` is a DOMException as `new DOMException(message, name)`
// would make it, but for the keys in `extra` and its stack's frames. With
// `stack`, its stack is that instead.
export function assertDomException(
  error,
  { name, message, code, stack },
  ...extra
) {
  ok(error instanceof DOMException);
  strictEqual(Object.getPrototypeOf(error), DOMException.prototype);
  strictEqual(error.constructor, DOMException);
  strictEqual(error.name, name);
  strictEqual(error.message, message);
  strictEqual(error.code, code);
  strictEqual(Error.isError(error), usingTsImpl);
  deepStrictEqual(Reflect.ownKeys(error).map(String), instanceKeys(...extra));
  if (stack === undefined) {
    strictEqual(typeof error.stack, 'string');
    strictEqual(error.stack.split('\n')[0], stackHeader(name, message));
  } else {
    strictEqual(error.stack, stack);
  }
}

// The first line of a stack, as Error.prototype.toString() makes it.
export function stackHeader(name, message) {
  return message === '' ? name : `${name}: ${message}`;
}

// An unpaired high surrogate.
export const kLoneSurrogate = '\uD800';
