// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (
      url.hostname == 'python-packages.edgeworker.net' ||
      url.hostname == 'pyodide-packages.runtime-playground.workers.dev'
    ) {
      return env.INTERNET.fetch(req);
    } else if (url.hostname == 'example.com') {
      return env.PYTHON.fetch(req);
    }

    throw new Error('Invalid url in proxy.js: ' + url);
  },
};
