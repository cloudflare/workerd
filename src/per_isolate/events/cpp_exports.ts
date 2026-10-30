'use strict';

const { ObjectFreeze } = primordials;

const { cppExports: eventCppExports } = require('events/event');
const { cppExports: eventTargetCppExports } = require('events/event-target');

module.exports = ObjectFreeze({
  __proto__: null,
  ...eventCppExports,
  ...eventTargetCppExports,
});
