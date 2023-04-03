import { equal, throws, rejects, doesNotReject } from "node:assert";
import { openDoor } from "test:module";


export const test_module_api = {
  test() {
    throws(() => openDoor("test key"));
    equal(openDoor("0p3n s3sam3"), true);
  }
};

export const test_builtin_dynamic_import = {
  async test() {
    await doesNotReject(import("test:module"));
  }
}

// internal modules can't be imported
export const test_builtin_internal_dynamic_import = {
  async test() {
    await rejects(import("test-internal:internal-module"));
  }
}
