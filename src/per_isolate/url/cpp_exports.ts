'use strict';

// The functions the C++ bridge (api/url-bridge.c++) calls, looked up by name
// with tryGetBootstrapFunction(). Null-prototype, so a missing name fails
// the lookup rather than resolving through a polluted Object.prototype, and
// frozen.

import type { UrlExports } from './url';

const { ObjectFreeze } = primordials;

const { cppExports } = require('url/url') as UrlExports;

module.exports = ObjectFreeze({
  __proto__: null,
  ...cppExports,
});
