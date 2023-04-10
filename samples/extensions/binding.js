import { BurritoShop } from "burrito-shop-internal:burrito-shop-impl";

function makeBinding(env) {
  return new BurritoShop(env.recipes);
}

export default makeBinding;
