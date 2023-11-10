import add_module from "node-internal:add_wasm";

const instance = new WebAssembly.Instance(add_module);

export function add(x, y) {
  return instance.exports.add(x,y);
}
