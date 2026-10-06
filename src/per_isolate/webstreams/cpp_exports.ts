'use strict';

const { ObjectFreeze } = primordials;

const { cppExports: readableCppExports } = require('webstreams/readable');
const { cppExports: writableCppExports } = require('webstreams/writable');

// The C++ bridge looks names up here with an ordinary property get
// (getCppExport in js-streams-bridge.c++). The table has a null prototype,
// so a name missing from it fails the lookup instead of resolving through a
// polluted Object.prototype, and it is frozen, like the two halves.
module.exports = ObjectFreeze({
  __proto__: null,
  ...readableCppExports,
  ...writableCppExports,
});
