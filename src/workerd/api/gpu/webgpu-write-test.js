import { ok, deepEqual } from "node:assert";

// run manually for now
// bazel run --//src/workerd/io:enable_experimental_webgpu //src/workerd/server:workerd -- test `realpath ./src/workerd/api/gpu/webgpu-write-test.gpu-wd-test` --verbose --experimental

export const read_sync_stack = {
  async test(ctrl, env, ctx) {
    ok(navigator.gpu);
    const adapter = await navigator.gpu.requestAdapter();
    ok(adapter);
    const device = await adapter.requestDevice();
    ok(device);

    // Get a GPU buffer in a mapped state and an arrayBuffer for writing.
    const gpuWriteBuffer = device.createBuffer({
      mappedAtCreation: true,
      size: 4,
      usage: GPUBufferUsage.MAP_WRITE | GPUBufferUsage.COPY_SRC,
    });
    ok(gpuWriteBuffer);
    const arrayBuffer = gpuWriteBuffer.getMappedRange();
    ok(arrayBuffer);

    // Write bytes to buffer.
    new Uint8Array(arrayBuffer).set([0, 1, 2, 3]);
    deepEqual(new Uint8Array(arrayBuffer), new Uint8Array([ 0, 1, 2, 3 ]));
  },
};
