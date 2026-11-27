// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { default as zlibUtil, type ZlibConstants } from 'node-internal:zlib';

export const constants: ZlibConstants = zlibUtil.getConstants();

// Translation table for return codes.
const rawCodes: Record<string, number | string> = {
  Z_OK: constants.Z_OK,
  Z_STREAM_END: constants.Z_STREAM_END,
  Z_NEED_DICT: constants.Z_NEED_DICT,
  Z_ERRNO: constants.Z_ERRNO,
  Z_STREAM_ERROR: constants.Z_STREAM_ERROR,
  Z_DATA_ERROR: constants.Z_DATA_ERROR,
  Z_MEM_ERROR: constants.Z_MEM_ERROR,
  Z_BUF_ERROR: constants.Z_BUF_ERROR,
  Z_VERSION_ERROR: constants.Z_VERSION_ERROR,
};

for (const key of Object.keys(rawCodes)) {
  rawCodes[rawCodes[key] as number] = key;
}

export const codes = Object.freeze(rawCodes);
