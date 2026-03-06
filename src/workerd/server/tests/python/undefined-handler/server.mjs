// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { rejects } from 'node:assert';

export default {
  async test(ctrl, env) {
    await rejects(env.pythonWorker.fetch(new Request('http://example.com')), {
      name: 'Error',
      message:
        'Python entrypoint "undefined_handler.py" does not export a handler named "on_fetch"',
    });
  },
};
