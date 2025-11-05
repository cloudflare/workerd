// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

const dir = await navigator.storage.getDirectory();
const bundle = await dir.getDirectoryHandle("tmp");
const file = await bundle.getFileHandle("foo", { create: true });
const handle = await file.createSyncAccessHandle();

const enc = new TextEncoder();
handle.write(enc.encode("Hello World"));
handle.write(enc.encode("!!!\n"));
console.log(handle.getSize());
handle.close();

const data = await file.getFile();
console.log(await data.text());

export default {
  async fetch(req, env) {
    return new Response("Hello World\n");
  }
};
