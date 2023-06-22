import assert from "node:assert";
import rtti from "workerd:rtti";

export default {
  async test(ctrl, env, ctx) {
    const buffer = rtti.exportTypes("2023-05-18", ["nodejs_compat"]);
    assert(buffer.byteLength > 0);
  }
}
