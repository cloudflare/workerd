import { BurritoShop } from "burrito-shop:burrito-shop";

export default {
  async fetch(req, env) {
    const shop = new BurritoShop({
      "meat": ["rice", "meat", "beans", "cheese", "salsa"],
      "veggie": ["rice", "guacamole", "beans", "salsa"],
    });
    const burritoType = await req.text();
    return new Response(shop.makeBurrito(burritoType).price());
  }
};
