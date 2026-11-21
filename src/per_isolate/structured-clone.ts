'use strict';

// The global structuredClone(), installed by main.ts on
// ServiceWorkerGlobalScope.prototype under the TS_STRUCTURED_CLONE autogate, in
// place of the C++ JSG method.
//
// This module is the WebIDL layer: the receiver check, the argument count, and
// the conversion of the StructuredSerializeOptions dictionary. The clone itself
// is the native serializer's, through utils.structuredClone(), so which values
// clone, which objects transfer, and the errors for either are the same as in
// C++. Where the conversion differs from C++ it follows WebIDL; the differences
// are tabled in src/tests/structured-clone/AGENTS.md.

import type { JsgErrorMessages } from './jsg-errors';

const {
  ArrayPrototypePush,
  FunctionPrototypeCall,
  ObjectDefineProperty,
  SymbolIterator,
  TypeError: TypeErrorCtor,
} = primordials;

const { structuredClone: nativeStructuredClone } = utils;

// The global proxy, which is the receiver of `globalThis.structuredClone(v)`.
const globalReceiver: unknown = globalThis;

// The C++ texts, kept where the behaviour matches.
const { kIllegalInvocation: ILLEGAL_INVOCATION } =
  require('jsg-errors') as JsgErrorMessages;
const NO_VALUE =
  "Failed to execute 'structuredClone' on 'ServiceWorkerGlobalScope': " +
  "parameter 1 is not of type 'Value'.";
const BAD_OPTIONS =
  "Failed to execute 'structuredClone' on 'ServiceWorkerGlobalScope': " +
  "parameter 2 is not of type 'StructuredCloneOptions'.";
const BAD_TRANSFER =
  "Incorrect type for the 'transfer' field on 'StructuredCloneOptions': " +
  "the provided value is not of type 'sequence<object>'.";
const BAD_TRANSFER_ITEM =
  "Incorrect type for an element of the 'transfer' field on " +
  "'StructuredCloneOptions': the provided value is not of type 'object'.";
// After V8's texts for the same failures in for...of.
const ITERATOR_NOT_OBJECT =
  'Result of the Symbol.iterator method is not an object';
const RESULT_NOT_OBJECT = 'Iterator result is not an object';

function isObject(value: unknown): value is object {
  return (
    (typeof value === 'object' && value !== null) || typeof value === 'function'
  );
}

// WebIDL's conversion to sequence<object>: any iterable, consumed through its
// own iterator (the user's, by contract), into an array of our own that the
// native side can read without running user code. Spelled out rather than
// written as for...of, which differs from WebIDL's "create a sequence from an
// iterable" in two observable ways: it reads [Symbol.iterator] a second time
// after the check, and it closes the iterator when an element is rejected.
function toObjectSequence(value: unknown): object[] {
  if (!isObject(value)) {
    throw new TypeErrorCtor(BAD_TRANSFER);
  }
  const method: unknown = (value as Record<symbol, unknown>)[SymbolIterator];
  if (typeof method !== 'function') {
    throw new TypeErrorCtor(BAD_TRANSFER);
  }
  const iterator: unknown = FunctionPrototypeCall(method, value);
  if (!isObject(iterator)) {
    throw new TypeErrorCtor(ITERATOR_NOT_OBJECT);
  }
  const next = (iterator as { next?: unknown }).next;
  const list: object[] = [];
  for (;;) {
    const result: unknown = FunctionPrototypeCall(
      next as (this: unknown) => unknown,
      iterator
    );
    if (!isObject(result)) {
      throw new TypeErrorCtor(RESULT_NOT_OBJECT);
    }
    if ((result as { done?: unknown }).done) {
      return list;
    }
    const item = (result as { value?: unknown }).value;
    if (!isObject(item)) {
      throw new TypeErrorCtor(BAD_TRANSFER_ITEM);
    }
    ArrayPrototypePush(list, item);
  }
}

// A method, so the function has no `prototype` and is not a constructor, like
// the JSG method it replaces.
const { structuredClone } = {
  structuredClone(this: unknown, value: unknown, options?: unknown): unknown {
    // WebIDL: an operation of the global takes undefined or null as the
    // global itself.
    if (this != null && this !== globalReceiver) {
      throw new TypeErrorCtor(ILLEGAL_INVOCATION);
    }
    if (arguments.length === 0) {
      throw new TypeErrorCtor(NO_VALUE);
    }
    let transfer: object[] | undefined;
    if (options != null) {
      if (!isObject(options)) {
        throw new TypeErrorCtor(BAD_OPTIONS);
      }
      const list = (options as { transfer?: unknown }).transfer;
      if (list !== undefined) {
        transfer = toObjectSequence(list);
      }
    }
    return nativeStructuredClone(value, transfer);
  },
};

// WebIDL: the optional options argument does not count.
ObjectDefineProperty(structuredClone, 'length', {
  __proto__: null,
  configurable: true,
  enumerable: false,
  writable: false,
  value: 1,
});

module.exports = { structuredClone };
