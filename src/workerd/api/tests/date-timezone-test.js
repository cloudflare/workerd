// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async test(ctrl, env, ctx) {
    const date = new Date('2023-05-15T12:00:00Z');
    const hours = date.getHours();
    const utcHours = date.getUTCHours();

    if (hours !== utcHours) {
      throw new Error(
        `Date API should always use UTC: getHours() ${hours} should equal getUTCHours() ${utcHours}`
      );
    }
  },
};
