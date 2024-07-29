// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { default as Stdin } from "workerd:stdin";
import { default as util } from "node:util";

export default {
  async test() {
    Stdin.reprl();
  }
};
