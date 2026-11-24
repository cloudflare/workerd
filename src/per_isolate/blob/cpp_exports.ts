'use strict';

// What the C++ bridge (api/blob-bridge.c++) reads to make an instance
// without running JavaScript: the classes' prototypes, and whether
// instances have their attributes as own properties. Null-prototype, so a
// missing name fails the lookup rather than resolving through a polluted
// Object.prototype, and frozen. Loaded only under the TS_BLOB autogate,
// which the bridge detects by whether it is.

import type { BlobExports } from './blob';

const { ObjectFreeze } = primordials;

const { cppExports } = require('blob/blob') as BlobExports;

module.exports = ObjectFreeze({
  __proto__: null,
  ...cppExports,
});
