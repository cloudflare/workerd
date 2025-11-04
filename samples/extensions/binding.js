// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { BurritoShop } from "burrito-shop-internal:burrito-shop-impl";

function makeBinding(env) {
  return new BurritoShop(env.recipes);
}

export default makeBinding;
