// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, throws } from 'node:assert';
import { inspect } from 'node:util';

export const test1 = {
  async test(ctrl, env, ctx) {
    let blob = new Blob(["foo", new TextEncoder().encode("bar"), "baz"]);
    strictEqual(await blob.text(), "foobarbaz");
    strictEqual(new TextDecoder().decode(await blob.arrayBuffer()), "foobarbaz");
    strictEqual(blob.type, "");

    let blob2 = new Blob(["xx", blob, "yy", blob], {type: "application/whatever"});
    strictEqual(await blob2.text(), "xxfoobarbazyyfoobarbaz");
    strictEqual(blob2.type, "application/whatever");

    let blob3 = new Blob();
    strictEqual(await blob3.text(), "");

    let slice = blob2.slice(5, 16);
    strictEqual(await slice.text(), "barbazyyfoo");
    strictEqual(slice.type, "");

    let slice2 = slice.slice(-5, 1234, "type/type");
    strictEqual(await slice2.text(), "yyfoo");
    strictEqual(slice2.type, "type/type");

    strictEqual(await blob2.slice(5).text(), "barbazyyfoobarbaz");
    strictEqual(await blob2.slice().text(), "xxfoobarbazyyfoobarbaz");
    strictEqual(await blob2.slice(3, 1).text(), "");

    {
      let stream = blob.stream();
      let reader = stream.getReader();
      let readResult = await reader.read();
      strictEqual(readResult.done, false);
      strictEqual(new TextDecoder().decode(readResult.value), "foobarbaz");
      readResult = await reader.read();
      strictEqual(readResult.value, undefined);
      strictEqual(readResult.done, true);
      reader.releaseLock();
    }

    let before = Date.now();

    let file = new File([blob, "qux"], "filename.txt");
    strictEqual(file instanceof Blob, true);
    strictEqual(await file.text(), "foobarbazqux");
    strictEqual(file.name, "filename.txt");
    strictEqual(file.type, "");
    if (file.lastModified < before || file.lastModified > Date.now()) {
      throw new Error("incorrect lastModified");
    }

    let file2 = new File(["corge", file], "file2", {type: "text/foo", lastModified: 123});
    strictEqual(await file2.text(), "corgefoobarbazqux");
    strictEqual(file2.name, "file2");
    strictEqual(file2.type, "text/foo");
    strictEqual(file2.lastModified, 123);

    try {
      new Blob(["foo"], {endings: "native"});
      throw new Error("use of 'endings' should throw");
    } catch (err) {
      if (!err.message.includes("The 'endings' field on 'Options' is not implemented.")) {
        throw err;
      }
    }

    // Test type normalization.
    strictEqual(new Blob([], {type: "FoO/bAr"}).type, "foo/bar");
    strictEqual(new Blob([], {type: "FoO\u0019/bAr"}).type, "");
    strictEqual(new Blob([], {type: "FoO\u0020/bAr"}).type, "foo /bar");
    strictEqual(new Blob([], {type: "FoO\u007e/bAr"}).type, "foo\u007e/bar");
    strictEqual(new Blob([], {type: "FoO\u0080/bAr"}).type, "");
    strictEqual(new File([], "foo.txt", {type: "FoO/bAr"}).type, "foo/bar");
    strictEqual(blob2.slice(1, 2, "FoO/bAr").type, "foo/bar");
  }
};

export const test2 = {
  async test(ctrl, env, ctx) {
    // This test verifies that a Blob created from a request/response properly reflects
    // the content and content-type specified by the request/response.
    const res = await env['request-blob'].fetch('http://example.org/blob-request', {
      method: 'POST',
      body: 'abcd1234',
      headers: { 'content-type': 'some/type' }
    });

    strictEqual(res.headers.get('content-type'), 'return/type');
    strictEqual(await res.text(), 'foobarbaz');
  },
  async doTest(request) {
    let b = await request.blob();
    strictEqual(await b.text(), "abcd1234");
    strictEqual(b.type, "some/type");

    // Quick check that content-type header is correctly not set when the blob type is an empty
    // string.
    strictEqual(new Response(new Blob(["typeful"], {type: "foo"}))
                    .headers.has("Content-Type"), true);
    strictEqual(new Response(new Blob(["typeless"]))
                    .headers.has("Content-Type"), false);

    return new Response(new Blob(["foobar", "baz"], {type: "return/type"}));
  }
};

export default {
  async fetch(request) {
    if (request.url.endsWith('/blob-request')) {
      return test2.doTest(request);
    } else {
      throw new Error('Unexpected test request');
    }
  }
};

export const testInspect = {
  async test(ctrl, env, ctx) {
    const blob = new Blob(["abc"], { type: "text/plain" });
    strictEqual(inspect(blob), "Blob { size: 3, type: 'text/plain' }");

    const file = new File(["1"], "file.txt", { type: "text/plain", lastModified: 1000 });
    strictEqual(inspect(file), "File { name: 'file.txt', lastModified: 1000, size: 1, type: 'text/plain' }");
  }
};

export const overLarge = {
  test() {
    const blob1 = new Blob([new ArrayBuffer(128 * 1024 * 1024)]);

    throws(() => {
      new Blob([new ArrayBuffer((128 * 1024 * 1024) + 1)]);
    }, {
      message: 'Blob size 134217729 exceeds limit 134217728',
      name: 'RangeError',
    });

    throws(() => {
      new Blob([' ', blob1]);
    }, {
      message: 'Blob size 134217729 exceeds limit 134217728',
      name: 'RangeError',
    });
  }
};
