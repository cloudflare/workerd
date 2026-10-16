'use strict';

// Helpers shared by the events modules. Leaf module: requires nothing.
//
// TODO(cleanup): isObject, kNullProtoObject, kEnumerable,
// toStringTagProperty and toDictionary duplicate the WebIDL helpers in
// webstreams/common.ts (isObject is that file's requireObject check), and
// requireAbortSignal duplicates the signal check in webstreams/readable.ts's
// convertPipeOptions. Reconcile them into one shared module.

const { AbortSignalAbortedGet, ObjectFreeze, TypeError } = primordials;

// WebIDL's check that a value is an Object: true for functions too, false
// for null and every other primitive.
function isObject(value: unknown): value is object {
  return (
    (typeof value === 'object' && value !== null) || typeof value === 'function'
  );
}

// What an omitted (or null) dictionary argument stands for. Null-prototype:
// WebIDL reads nothing for an omitted dictionary, so neither may we.
const kNullProtoObject: object = ObjectFreeze({ __proto__: null });

const kEnumerable: PropertyDescriptor = ObjectFreeze({
  __proto__: kNullProtoObject,
  enumerable: true,
});

function toStringTagProperty(tag: string): PropertyDescriptor {
  return {
    __proto__: kNullProtoObject,
    value: tag,
    writable: false,
    enumerable: false,
    configurable: true,
  } as PropertyDescriptor;
}

// WebIDL conversion of an optional dictionary argument: undefined and null
// mean an empty dictionary, any other object (a function included) is read
// as the dictionary, and every other primitive is a TypeError.
function toDictionary(value: unknown, message: string): object {
  if (value == null) return kNullProtoObject;
  if (!isObject(value)) throw new TypeError(message);
  return value;
}

// WebIDL's TypeError for a call with fewer arguments than the operation
// requires. `context` names the call, as in "Failed to execute
// 'dispatchEvent' on 'EventTarget'".
function notEnoughArguments(
  context: string,
  required: number,
  present: number
): TypeError {
  const noun = required === 1 ? 'argument' : 'arguments';
  return new TypeError(
    `${context}: ${required} ${noun} required, but only ${present} present.`
  );
}

// WebIDL conversion of a value to an AbortSignal, returning whether it has
// aborted. Under the modern JSG layout the captured `aborted` getter throws
// for a receiver that is not an AbortSignal; under the instance-property
// layout (old compat dates) the capture is a plain read that cannot
// brand-check, so additionally require the boolean a genuine signal's own
// data property carries. Under old dates a forged {aborted: <boolean>}
// passes: utils.addAbortAlgorithm() rejects it when a listener is added, but
// a null listener, or a forgery claiming to have aborted, returns before
// that (event-target suite ledger #14).
function requireAbortSignal(value: unknown, message: string): boolean {
  let aborted: unknown;
  try {
    aborted = AbortSignalAbortedGet(value);
  } catch {
    throw new TypeError(message);
  }
  if (typeof aborted !== 'boolean') throw new TypeError(message);
  return aborted;
}

export type CommonHelpers = {
  isObject: typeof isObject;
  kNullProtoObject: typeof kNullProtoObject;
  kEnumerable: typeof kEnumerable;
  toStringTagProperty: typeof toStringTagProperty;
  toDictionary: typeof toDictionary;
  notEnoughArguments: typeof notEnoughArguments;
  requireAbortSignal: typeof requireAbortSignal;
};

module.exports = ObjectFreeze({
  isObject,
  kNullProtoObject,
  kEnumerable,
  toStringTagProperty,
  toDictionary,
  notEnoughArguments,
  requireAbortSignal,
});
