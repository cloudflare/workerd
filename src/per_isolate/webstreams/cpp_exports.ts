'use strict';

const { cppExports: readableCppExports } = require('webstreams/readable');
const { cppExports: writableCppExports } = require('webstreams/writable');

module.exports = {
  ...readableCppExports,
  ...writableCppExports,
};
