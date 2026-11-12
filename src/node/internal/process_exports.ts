// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

// What node:process resolves to when the enable_nodejs_process_v2 flag is set. The
// `process` global reads node-internal:public_process directly, which defers the stdio
// streams until they are accessed; an ES module cannot defer a named export, so
// importing node:process by name builds them, as it does in Node.js.

export * from 'node-internal:public_process';
export { default } from 'node-internal:public_process';
export { stdin, stdout, stderr } from 'node-internal:internal_process_stdio';
