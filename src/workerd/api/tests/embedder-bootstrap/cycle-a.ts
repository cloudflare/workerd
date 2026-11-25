'use strict';
// Requires cycle-b, which requires this script back: requiring either fails.

module.exports = { b: require('test-embedder/cycle-b') };
