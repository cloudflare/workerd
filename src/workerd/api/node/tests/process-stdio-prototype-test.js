// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This test must not import any node: module, so that the stdio module graph is
// first loaded inside a request, after worker code has modified Object.prototype.
// Each service in the .wd-test sets a different property name in its own isolate.

function check(cond, message) {
  if (!cond) throw new Error(message);
}

export default {
  async test(ctrl, env) {
    const name = env.PROPERTY;
    check(process.env, 'process.env should be available');
    Object.prototype[name] = undefined;
    try {
      check(process.stdout.fd === 1, `stdout.fd with Object.prototype.${name}`);
      check(process.stderr.fd === 2, `stderr.fd with Object.prototype.${name}`);
      check(process.stdin.fd === 0, `stdin.fd with Object.prototype.${name}`);
      const { StringDecoder } = await import('node:string_decoder');
      check(new StringDecoder('utf8').lastNeed === 0, 'StringDecoder.lastNeed');
      const zlib = await import('node:zlib');
      check(zlib.constants.Z_OK === 0, 'zlib.constants.Z_OK');
    } finally {
      delete Object.prototype[name];
    }
  },
};
