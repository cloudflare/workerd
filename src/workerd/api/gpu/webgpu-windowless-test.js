import { ok, deepEqual, equal } from "node:assert";

// run manually for now
// bazel run --//src/workerd/io:enable_experimental_webgpu //src/workerd/server:workerd -- test `realpath ./src/workerd/api/gpu/webgpu-windowless-test.gpu-wd-test` --verbose --experimental

export class DurableObjectExample {
  constructor(state) {
    this.state = state;
  }

  async fetch() {
    ok(navigator.gpu);
    const adapter = await navigator.gpu.requestAdapter({
      powerPreference: "high-performance",
      forceFallbackAdapter: false,
    });
    ok(adapter);

    const device = await adapter.requestDevice();
    ok(device);

    const texture = device.createTexture({
      size: { width: 256, height: 256, depthOrArrayLayers: 1 },
      format: "rgba8unorm-srgb",
      usage: GPUTextureUsage.COPY_SRC | GPUTextureUsage.RENDER_ATTACHMENT,
    });
    ok(texture);

    return new Response("OK");
  }
}

export const windowless = {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName("A");
    let obj = env.ns.get(id);
    let res = await obj.fetch("http://foo/test");
    let text = await res.text();
    equal(text, "OK");
  },
};
