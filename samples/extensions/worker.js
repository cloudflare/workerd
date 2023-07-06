// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { BurritoShop } from "burrito-shop:burrito-shop";

export default {
  async fetch(req, env) {
    const burritoType = await req.text();

    if (req.headers.has("X-Use-Direct-Api")) {
      // extensions can decide to provide direct API access to users
      const shop = new BurritoShop({
        "meat": ["rice", "meat", "beans", "cheese", "salsa"],
        "veggie": ["rice", "guacamole", "beans", "salsa"],
      });
      return new Response(shop.makeBurrito(burritoType).price());
    } else {
      // or can be used to define wrapped bindings,
      // which is more in line with capability-based design
      return new Response(env.shop.makeBurrito(burritoType).price());
    }
  }
};
