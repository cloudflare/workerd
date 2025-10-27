import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { ok, strictEqual } from 'node:assert';

// File system operations at the top level scope are allowed
const config = readFileSync('/bundle/config', 'utf8');
strictEqual(config, 'config file');

export default {
  async fetch(request) {
    // All files in the /tmp directory are always per-request.
    // When the request ends, the files are deleted.
    ok(!existsSync('/tmp/hello.txt'));
    writeFileSync('/tmp/hello.txt', 'Hello, World!', 'utf8');
    return new Response(readFileSync('/tmp/hello.txt'));
  },
};
