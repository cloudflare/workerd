// burrito-shop-impl is an internal module and can't be imported by user code


// internal modules can import each other, but users still can't access them
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
