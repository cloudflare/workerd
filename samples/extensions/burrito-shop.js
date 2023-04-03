// kitchen is hidden from the user
import { makeBurritoImpl } from "burrito-shop-internal:kitchen";

export class BurritoShop {
  #recipes;

  constructor(recipes) {
    this.#recipes = recipes;
  }

  makeBurrito(type) {
    if (!type in this.#recipes) {
      throw new Error(`recipe not found: ${type}`);
    }
    return makeBurritoImpl(this.#recipes[type]);
  }
}
