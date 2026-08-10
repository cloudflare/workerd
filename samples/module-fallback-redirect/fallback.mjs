// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { createServer } from 'node:http';

function sendJson(response, value) {
  response.writeHead(200, { 'content-type': 'application/json' });
  response.end(JSON.stringify(value));
}

createServer(async (request, response) => {
  let body = '';
  for await (const chunk of request) {
    body += chunk;
  }

  const resolution = JSON.parse(body);
  console.log(resolution);

  switch (resolution.specifier) {
    case 'file:///bundle/static-test-dependency':
      response.writeHead(301, {
        location:
          'file:///project/node_modules/static-test-dependency/index.mjs',
      });
      response.end();
      return;
    case 'file:///project/node_modules/static-test-dependency/index.mjs':
      sendJson(response, {
        esModule: [
          'import { value } from "./value.mjs";',
          'export { value };',
        ].join('\n'),
      });
      return;
    case 'file:///bundle/value.mjs':
      response.writeHead(301, {
        location:
          'file:///project/node_modules/static-test-dependency/value.mjs',
      });
      response.end();
      return;
    case 'file:///project/node_modules/static-test-dependency/value.mjs':
      sendJson(response, {
        esModule: 'export const value = "loaded";',
      });
      return;
    case 'file:///bundle/dynamic-test-dependency':
      response.writeHead(301, {
        location:
          'file:///project/node_modules/dynamic-test-dependency/index.mjs',
      });
      response.end();
      return;
    case 'file:///project/node_modules/dynamic-test-dependency/index.mjs':
      sendJson(response, {
        esModule: [
          'export async function load() {',
          '  return await import("./value.mjs");',
          '}',
        ].join('\n'),
      });
      return;
    case 'file:///project/node_modules/dynamic-test-dependency/value.mjs':
      sendJson(response, {
        esModule: 'export const value = "loaded";',
      });
      return;
    default:
      response.writeHead(404);
      response.end();
  }
}).listen(8888, '127.0.0.1', () => {
  console.log('Fallback service listening on http://127.0.0.1:8888');
});
