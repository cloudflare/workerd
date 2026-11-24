'use strict';

// The message of the TypeError C++ JSG throws when a method or accessor is
// called on an object that is not of its type (jsg/resource.h).
const kIllegalInvocation =
  'Illegal invocation: function called with incorrect `this` reference. ' +
  'See https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';

const jsgErrors = { kIllegalInvocation };
export type JsgErrorMessages = typeof jsgErrors;

module.exports = jsgErrors;
