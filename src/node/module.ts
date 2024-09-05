// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { default as moduleUtil } from 'node-internal:module';
import { ERR_INVALID_ARG_VALUE } from 'node-internal:internal_errors';

export function createRequire(
  path: string | URL
): (specifier: string) => unknown {
  // Note that per Node.js' requirements, path must be one of either
  // an absolute file path or a file URL. We do not currently handle
  // module specifiers as URLs yet, but we'll try to get close.

  const normalizedPath = `${path}`;
  if (!normalizedPath.startsWith('/') && !normalizedPath.startsWith('file:')) {
    throw new ERR_INVALID_ARG_VALUE(
      'path',
      normalizedPath,
      'The argument must be a file URL object, a file URL string, or an absolute path string.'
    );
  }

  return moduleUtil.createRequire(normalizedPath);
}

export default {
  createRequire,
};
