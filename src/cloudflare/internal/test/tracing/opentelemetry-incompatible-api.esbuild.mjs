// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { readFile } from 'node:fs/promises';

// Exercise the real package's compatibility handshake, changing only its consumer version.
export default {
  plugins: [
    {
      name: 'simulate-api-1.11',
      setup(build) {
        let patched = false;
        build.onLoad({ filter: /[/\\]version\.mjs$/ }, async ({ path }) => {
          const source = await readFile(path, 'utf8');
          const contents = source.replace(
            'const VERSION = "1.10.0";',
            'const VERSION = "1.11.0";'
          );
          if (contents === source)
            throw new Error('API version fixture no longer matches');
          patched = true;
          return { contents, loader: 'js' };
        });
        build.onEnd(() => {
          if (!patched)
            return {
              errors: [{ text: 'API version fixture was not patched' }],
            };
        });
      },
    },
  ],
};
