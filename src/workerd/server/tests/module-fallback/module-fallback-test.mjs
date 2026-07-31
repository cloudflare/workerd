// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtemp, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';

assert(env.WORKERD_BINARY, 'WORKERD_BINARY must be set');
assert(env.TEST_TMPDIR, 'TEST_TMPDIR must be set');

const NMR_WORKER = `
function equal(actual, expected) {
  if (actual !== expected) {
    throw new Error(\`Expected \${JSON.stringify(expected)}, got \${JSON.stringify(actual)}\`);
  }
}

async function rejects(promise, expected) {
  try {
    await promise;
  } catch (error) {
    if (!String(error).includes(expected)) throw error;
    return;
  }
  throw new Error(\`Expected rejection containing: \${expected}\`);
}

export const fallbackModules = {
  async test() {
    const esm = await import('./esm.js');
    equal(esm.default, 'esm fallback');
    equal(esm.named, 7);

    const text = await import('./text.txt');
    equal(text.default, 'text fallback');

    const data = new Uint8Array((await import('./data.bin')).default);
    equal(Array.from(data).join(','), '0,1,127,255');

    const json = await import('./value.json', { with: { type: 'json' } });
    equal(json.default.answer, 42);

    const common = await import('./common.cjs');
    equal(common.named, 'required fallback');

    const wasm = await import.source('./module.wasm');
    if (!(wasm instanceof WebAssembly.Module)) {
      throw new Error('Expected fallback Wasm source to be a WebAssembly.Module');
    }

    const redirected = await import('./redirect.js');
    equal(redirected.default, 'redirect fallback');

    await rejects(
      import('./python.py'),
      'Module not found: file:///bundle/python.py'
    );
    await rejects(
      import('./invalid.js'),
      'Module not found: file:///bundle/invalid.js'
    );
    await rejects(
      import('./missing.js'),
      'Module not found: file:///bundle/missing.js'
    );
  },
};
`;

const LEGACY_WORKER = `
export const legacyFallback = {
  async test() {
    const module = await import('./legacy.js');
    if (module.default !== 'legacy fallback') {
      throw new Error('Legacy fallback returned the wrong value');
    }
  },
};
`;

function sendJson(response, value) {
  response.writeHead(200, { 'content-type': 'application/json' });
  response.end(JSON.stringify(value));
}

async function runWorkerd(configPath) {
  return await new Promise((resolve, reject) => {
    let output = '';
    const child = spawn(
      env.WORKERD_BINARY,
      ['test', configPath, '--experimental'],
      { stdio: ['ignore', 'pipe', 'pipe'] }
    );
    const timeout = setTimeout(() => {
      child.kill('SIGKILL');
      reject(new Error(`workerd test timed out:\n${output}`));
    }, 30_000);

    child.stdout.on('data', (data) => (output += data));
    child.stderr.on('data', (data) => (output += data));
    child.once('error', (error) => {
      clearTimeout(timeout);
      reject(error);
    });
    child.once('close', (code, signal) => {
      clearTimeout(timeout);
      resolve({ code, signal, output });
    });
  });
}

test('module fallback serves V1 and V2 module types', async () => {
  const requests = [];
  const handlerErrors = [];

  const fallback = createServer((request, response) => {
    void (async () => {
      let body = '';
      for await (const chunk of request) body += chunk;

      if (request.method === 'GET') {
        const url = new URL(request.url, 'http://fallback.invalid');
        requests.push({
          version: 'v1',
          method: request.headers['x-resolve-method'],
          specifier: url.searchParams.get('specifier'),
          rawSpecifier: url.searchParams.get('rawSpecifier'),
          referrer: url.searchParams.get('referrer'),
        });
        sendJson(response, { esModule: "export default 'legacy fallback';" });
        return;
      }

      assert.equal(request.method, 'POST');
      const resolution = JSON.parse(body);
      requests.push({ version: 'v2', ...resolution });

      switch (resolution.specifier) {
        case 'file:///bundle/esm.js':
          sendJson(response, {
            esModule: "export default 'esm fallback'; export const named = 7;",
          });
          return;
        case 'file:///bundle/text.txt':
          sendJson(response, { text: 'text fallback' });
          return;
        case 'file:///bundle/data.bin':
          sendJson(response, { data: [0, 1, 127, 255] });
          return;
        case 'file:///bundle/value.json':
          sendJson(response, { json: '{"answer":42}' });
          return;
        case 'file:///bundle/common.cjs':
          sendJson(response, {
            commonJsModule:
              "module.exports = { named: require('./required.txt') };",
            namedExports: ['named'],
          });
          return;
        case 'file:///bundle/required.txt':
          sendJson(response, { text: 'required fallback' });
          return;
        case 'file:///bundle/module.wasm':
          sendJson(response, { wasm: [0, 97, 115, 109, 1, 0, 0, 0] });
          return;
        case 'file:///bundle/redirect.js':
          response.writeHead(301, {
            location: 'file:///bundle/redirect-target.js',
          });
          response.end();
          return;
        case 'file:///bundle/redirect-target.js':
          sendJson(response, {
            esModule: "export default 'redirect fallback';",
          });
          return;
        case 'file:///bundle/python.py':
          sendJson(response, { pythonModule: 'x = 1' });
          return;
        case 'file:///bundle/invalid.js':
          sendJson(response, {
            name: 'not a valid module id',
            esModule: 'export default 1;',
          });
          return;
        case 'file:///bundle/missing.js':
          response.writeHead(404);
          response.end('not found');
          return;
        default:
          response.writeHead(500);
          response.end(`Unexpected specifier: ${resolution.specifier}`);
      }
    })().catch((error) => {
      handlerErrors.push(error);
      if (!response.headersSent) response.writeHead(500);
      response.end();
    });
  });

  await new Promise((resolve, reject) => {
    fallback.once('error', reject);
    fallback.listen(0, '127.0.0.1', resolve);
  });

  try {
    const address = fallback.address();
    assert(address && typeof address !== 'string');
    const directory = await mkdtemp(join(env.TEST_TMPDIR, 'module-fallback-'));
    const nmrWorkerPath = join(directory, 'nmr-main.js');
    const legacyWorkerPath = join(directory, 'legacy-main.js');
    const configPath = join(directory, 'config.capnp');
    await writeFile(nmrWorkerPath, NMR_WORKER);
    await writeFile(legacyWorkerPath, LEGACY_WORKER);
    await writeFile(
      configPath,
      `using Workerd = import "/workerd/workerd.capnp";

const unitTests :Workerd.Config = (
  services = [
    ( name = "nmr",
      worker = (
        modules = [(name = "nmr-main.js", esModule = embed "nmr-main.js")],
        compatibilityDate = "2026-07-01",
        compatibilityFlags = ["new_module_registry"],
        moduleFallback = "127.0.0.1:${address.port}",
      ),
    ),
    ( name = "legacy",
      worker = (
        modules = [(name = "legacy-main.js", esModule = embed "legacy-main.js")],
        compatibilityDate = "2026-07-01",
        moduleFallback = "127.0.0.1:${address.port}",
      ),
    ),
  ],
);
`
    );

    const result = await runWorkerd(configPath);
    assert.equal(
      result.code,
      0,
      `workerd exited with code=${result.code}, signal=${result.signal}:\n${result.output}`
    );
    assert.equal(handlerErrors.length, 0, String(handlerErrors[0]));
    assert.match(result.output, /Fallback service returned a Python module/);
    assert.match(
      result.output,
      /returned module name does not match specifier/
    );
    assert.match(result.output, /Fallback service failed to fetch module/);

    const v1 = requests.filter((request) => request.version === 'v1');
    assert.deepEqual(v1, [
      {
        version: 'v1',
        method: 'import',
        specifier: '/legacy.js',
        rawSpecifier: './legacy.js',
        referrer: '/legacy-main.js',
      },
    ]);

    const v2 = requests.filter((request) => request.version === 'v2');
    assert.equal(v2.length, 12);
    const jsonRequest = v2.find(
      (request) => request.specifier === 'file:///bundle/value.json'
    );
    assert.deepEqual(jsonRequest, {
      version: 'v2',
      type: 'import',
      specifier: 'file:///bundle/value.json',
      rawSpecifier: './value.json',
      referrer: 'file:///bundle/nmr-main.js',
      attributes: [{ name: 'type', value: 'json' }],
    });

    const requireRequest = v2.find(
      (request) => request.specifier === 'file:///bundle/required.txt'
    );
    assert.equal(requireRequest?.type, 'require');
    assert.equal(requireRequest?.rawSpecifier, './required.txt');
    assert.equal(requireRequest?.referrer, 'file:///bundle/common.cjs');
  } finally {
    await new Promise((resolve, reject) => {
      fallback.close((error) => (error ? reject(error) : resolve()));
    });
  }
});
