'use strict';

// The globals atob() and btoa() (HTML's WindowOrWorkerGlobalScope), as
// main.ts installs them under the TS_ATOB_BTOA autogate. The receiver and
// the argument are checked and converted here, as JSG does for the C++
// methods (api/global-scope.h), with its messages; simdutf does the base64
// behind utils.atob() and utils.btoa().

import type { DomExceptionModule } from './dom-exception';
import type { RuntimeClassHelpers } from './runtime-class';

const {
  ErrorCaptureStackTrace,
  ObjectDefineProperty,
  ObjectGetOwnPropertyDescriptor,
  TypeError: TypeErrorCtor,
} = primordials;

const { DOMException } = require('dom-exception') as DomExceptionModule;
const { setNameAndLength } = require('runtime-class') as RuntimeClassHelpers;

const { atob: decodeBase64, btoa: encodeBase64 } = utils;

const kGlobal = globalThis;

const kIllegalInvocation =
  'Illegal invocation: function called with incorrect `this` reference. ' +
  'See https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';
// JSG names the parameter's type after the C++ one, which differs.
const kAtobMissingArgument =
  "Failed to execute 'atob' on 'ServiceWorkerGlobalScope': parameter 1 is " +
  "not of type 'string'.";
const kBtoaMissingArgument =
  "Failed to execute 'btoa' on 'ServiceWorkerGlobalScope': parameter 1 is " +
  "not of type 'String'.";
const kSymbolArgument = 'Cannot convert a Symbol value to a string';
const kInvalidBase64 =
  'atob() called with invalid base64-encoded data. (Only whitespace, ' +
  "'+', '/', alphanumeric ASCII, and up to two terminal '=' signs when " +
  'the input data length is divisible by 4 are allowed.)';
const kNotLatin1 =
  'btoa() can only operate on characters in the Latin1 (ISO/IEC 8859-1) ' +
  'range.';

type GlobalFunction = (this: unknown, data: unknown) => string;

// The errors' stacks start at the code that called `fn`, the global
// function throwing them, as those of a native function's errors do.

function typeError(message: string, fn: GlobalFunction): TypeError {
  const error = new TypeErrorCtor(message);
  ErrorCaptureStackTrace(error, fn);
  return error;
}

function invalidCharacterError(message: string, fn: GlobalFunction): object {
  const error = new DOMException(message, 'InvalidCharacterError');
  // The C++ DOMException's own `stack` is enumerable, which capturing a
  // stack anew does not keep.
  const enumerable =
    ObjectGetOwnPropertyDescriptor(error, 'stack')?.enumerable === true;
  ErrorCaptureStackTrace(error, fn);
  if (enumerable) {
    ObjectDefineProperty(error, 'stack', { __proto__: null, enumerable: true });
  }
  return error;
}

// The argument as a string, after the checks JSG makes first and in its
// order: the receiver is the global or absent, and there is an argument.
function toStringArgument(
  self: unknown,
  argumentCount: number,
  data: unknown,
  missingArgument: string,
  fn: GlobalFunction
): string {
  if (self !== undefined && self !== null && self !== kGlobal) {
    throw typeError(kIllegalInvocation, fn);
  }
  if (argumentCount === 0) throw typeError(missingArgument, fn);
  if (typeof data === 'string') return data;
  // ToString() throws this itself, with a stack that begins here.
  if (typeof data === 'symbol') throw typeError(kSymbolArgument, fn);
  return `${data as string}`;
}

// Methods, which unlike function declarations are not constructors.
const functions = {
  atob(this: unknown, data: unknown): string {
    const result = decodeBase64(
      toStringArgument(
        this,
        arguments.length,
        data,
        kAtobMissingArgument,
        functions.atob
      )
    );
    if (result === undefined) {
      throw invalidCharacterError(kInvalidBase64, functions.atob);
    }
    return result;
  },

  btoa(this: unknown, data: unknown): string {
    const result = encodeBase64(
      toStringArgument(
        this,
        arguments.length,
        data,
        kBtoaMissingArgument,
        functions.btoa
      )
    );
    if (result === undefined) {
      throw invalidCharacterError(kNotLatin1, functions.btoa);
    }
    return result;
  },
};

// JSG counts the required argument only under
// spec_compliant_property_attributes.
const kLength = compatFlags['spec_compliant_property_attributes'] ? 1 : 0;
setNameAndLength(functions.atob, 'atob', kLength);
setNameAndLength(functions.btoa, 'btoa', kLength);

module.exports = functions;
