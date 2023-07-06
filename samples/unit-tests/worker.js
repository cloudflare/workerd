// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Exporting a test is like exporting a `fetch()` handler.
//
// It's common to want to write multiple tests in one file. We can export each
// test under a different name. (BTW you can also export multiple `fetch()`
// handlers this way! But that's less commonly-used.)

export let testStrings = {
  async test(ctrl, env, ctx) {
    if ("foo" + "bar" != "foobar") {
      throw new Error("strings are broken!");
    }
  }
};

export let testMath = {
  async test(ctrl, env, ctx) {
    if (1 + 1 != 2) {
      throw new Error("math is broken!");
    }
  }
};
