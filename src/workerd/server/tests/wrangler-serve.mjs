// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve` (or `dev`) of a directory's wrangler.jsonc, for the tests
// of wrangler.json fields.

import { spawn } from 'node:child_process';
import { join, resolve as absolute } from 'node:path';
import { env } from 'node:process';

const CONTROL_FD = 3;

// Runs workerd `command` on `dir`'s wrangler.jsonc in `cwd`, with `vars` added
// to its environment, until it listens or until it exits. Answers {port, stop,
// stderr, relisten}, whose stderr() is complete once stop() resolves and whose
// relisten() resolves to the port of the next listen, after a --watch reload;
// or {code, stderr}.
export function serve(dir, args, vars = {}, { command = 'serve', cwd } = {}) {
  const child = spawn(
    absolute(env.WORKERD_BINARY),
    [
      command,
      join(dir, 'wrangler.jsonc'),
      `--control-fd=${CONTROL_FD}`,
      '--socket-addr=http=127.0.0.1:0',
      ...args,
    ],
    {
      cwd,
      stdio: ['ignore', 'inherit', 'pipe', 'pipe'],
      env: { ...env, ...vars },
    }
  );
  let stderr = '';
  child.stderr.on('data', (d) => {
    stderr += d;
    process.stderr.write(d);
  });
  const exited = new Promise((resolve) => child.once('close', resolve));
  const stop = () => (child.kill(), exited);
  let relistened = () => {};
  const relisten = () => new Promise((r) => (relistened = r));
  return new Promise((resolve, reject) => {
    let buffer = '';
    child.stdio[CONTROL_FD].on('data', (d) => {
      buffer += d;
      for (const line of buffer.split('\n').slice(0, -1)) {
        const m = JSON.parse(line);
        if (m.event === 'listen') {
          relistened(m.port);
          resolve({ port: m.port, stop, stderr: () => stderr, relisten });
        }
      }
      buffer = buffer.slice(buffer.lastIndexOf('\n') + 1);
    });
    child.once('error', reject);
    exited.then((code) => resolve({ code, stderr }));
  });
}
