'use strict';

// The functions the C++ bridge (tryCallCryptoCppExport() in
// api/crypto/crypto.c++) calls, looked up by name with
// tryGetBootstrapFunction(). Null-prototype, so a missing name fails the
// lookup rather than resolving through a polluted Object.prototype, and
// frozen.

import type { CryptoExports } from './crypto';

const { ObjectFreeze } = primordials;

const { getCrypto } = require('crypto/crypto') as CryptoExports;

module.exports = ObjectFreeze({
  __proto__: null,
  getCrypto,
});
