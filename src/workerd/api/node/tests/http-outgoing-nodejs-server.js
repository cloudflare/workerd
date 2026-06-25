// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This is a sidecar that runs alongside the http-client-nodejs-test.* tests.
// It is executed using the appropriate Node.js version defined in build/deps/nodejs.MODULE.bazel.
const http = require('node:http');
const assert = require('node:assert/strict');

function listenAndReport(server, name) {
  server.listen({ port: 0, host: process.env.SIDECAR_HOSTNAME }, () => {
    console.log(`${name}=${server.address().port}`);
  });
}

const finishWritableServer = http.createServer((req, res) => {
  assert.strictEqual(res.writable, true);
  assert.strictEqual(res.finished, false);
  assert.strictEqual(res.writableEnded, false);
  res.end();

  // res.writable is set to false after it has finished sending
  // Ref: https://github.com/nodejs/node/issues/15029
  assert.strictEqual(res.writable, true);
  assert.strictEqual(res.finished, true);
  assert.strictEqual(res.writableEnded, true);
});
listenAndReport(finishWritableServer, 'FINISH_WRITABLE_PORT');

const writableFinishedServer = http.createServer((req, res) => {
  assert.strictEqual(res.writableFinished, false);
  res.on('finish', () => {
    assert.strictEqual(res.writableFinished, true);
  });
  res.end();
});
listenAndReport(writableFinishedServer, 'WRITABLE_FINISHED_PORT');

const propertiesServer = http.createServer((req, res) => {
  res.end();
});
listenAndReport(propertiesServer, 'PROPERTIES_PORT');
