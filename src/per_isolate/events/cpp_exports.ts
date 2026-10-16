'use strict';

const { ObjectFreeze } = primordials;

const { cppExports: eventTargetCppExports } = require('events/event-target');

module.exports = ObjectFreeze({
  __proto__: null,
  ...eventTargetCppExports,
});
