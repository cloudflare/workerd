import { BurritoShop } from "burrito-shop-internal:burrito-shop-impl";

export function wrapBindings(env) {
  return new BurritoShop(env.recipes);
}
