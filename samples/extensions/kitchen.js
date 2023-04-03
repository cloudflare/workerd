// kitchen.js is an internal module and can't be imported by user code.

const prices = { "rice": 1, "meat": 5, "beans": 1, "cheese": 1, "salsa": 1, "guacamole": 6, };

class Burrito {
  #recipe;
  constructor(recipe) {
    this.#recipe = recipe;
  }

  price() {
    return this.#recipe.reduce((acc, val) => acc + prices[val], 0);
  }
};

export function makeBurritoImpl(recipe) {
  return new Burrito(recipe);
}
