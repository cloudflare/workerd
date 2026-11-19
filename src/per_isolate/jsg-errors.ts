'use strict';

// The messages of the TypeErrors C++ JSG throws (typeErrorMessage() in
// jsg/util.c++, and jsg/resource.{h,c++}), for bootstrap code that replaces a
// C++ API and keeps its errors. Positions count from 1, as the messages do.

// A method or accessor called on an object that is not of its type.
const kIllegalInvocation =
  'Illegal invocation: function called with incorrect `this` reference. ' +
  'See https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';

function methodParameterMessage(
  type: string,
  method: string,
  position: number,
  expected: string
): string {
  return (
    `Failed to execute '${method}' on '${type}': parameter ${position} is ` +
    `not of type '${expected}'.`
  );
}

function constructorParameterMessage(
  type: string,
  position: number,
  expected: string
): string {
  return (
    `Failed to construct '${type}': constructor parameter ${position} is ` +
    `not of type '${expected}'.`
  );
}

function setterValueMessage(
  type: string,
  property: string,
  expected: string
): string {
  return (
    `Failed to set the '${property}' property on '${type}': the provided ` +
    `value is not of type '${expected}'.`
  );
}

// A field of a dictionary (JSG_STRUCT) argument.
function structFieldMessage(
  type: string,
  field: string,
  expected: string
): string {
  return (
    `Incorrect type for the '${field}' field on '${type}': the provided ` +
    `value is not of type '${expected}'.`
  );
}

export type JsgErrorMessages = {
  kIllegalInvocation: typeof kIllegalInvocation;
  methodParameterMessage: typeof methodParameterMessage;
  constructorParameterMessage: typeof constructorParameterMessage;
  setterValueMessage: typeof setterValueMessage;
  structFieldMessage: typeof structFieldMessage;
};

module.exports = {
  kIllegalInvocation,
  methodParameterMessage,
  constructorParameterMessage,
  setterValueMessage,
  structFieldMessage,
};
