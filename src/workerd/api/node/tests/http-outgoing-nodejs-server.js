// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This is a sidecar that runs alongside the http-client-nodejs-test.* tests.
// It is executed using the appropriate Node.js version defined in WORKSPACE.
const http = require('node:http');
const assert = require('node:assert/strict');

function reportPort(server) {
  console.info(`Listening on port ${server.address().port}`);
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
finishWritableServer.listen(process.env.FINISH_WRITABLE_PORT, () =>
  reportPort(finishWritableServer)
);

const writableFinishedServer = http.createServer((req, res) => {
  assert.strictEqual(res.writableFinished, false);
  res.on('finish', () => {
    assert.strictEqual(res.writableFinished, true);
  });
  res.end();
});
writableFinishedServer.listen(process.env.WRITABLE_FINISHED_PORT, () =>
  reportPort(writableFinishedServer)
);

const propertiesServer = http.createServer((req, res) => {
  res.end();
});
propertiesServer.listen(process.env.PROPERTIES_PORT, () =>
  reportPort(propertiesServer)
);
