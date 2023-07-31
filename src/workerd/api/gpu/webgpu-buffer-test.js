import { deepEqual, ok } from "node:assert";

// run manually for now
// bazel run --//src/workerd/io:enable_experimental_webgpu //src/workerd/server:workerd -- test `realpath ./src/workerd/api/gpu/webgpu-buffer-test.gpu-wd-test` --verbose --experimental

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

    // Unmap buffer so that it can be used later for copy.
    gpuWriteBuffer.unmap();

    // Get a GPU buffer for reading in an unmapped state.
    const gpuReadBuffer = device.createBuffer({
      mappedAtCreation: false,
      size: 4,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    ok(gpuReadBuffer);

    // Encode commands for copying buffer to buffer.
    const copyEncoder = device.createCommandEncoder();
    ok(copyEncoder);
    copyEncoder.copyBufferToBuffer(
      gpuWriteBuffer /* source buffer */,
      0 /* source offset */,
      gpuReadBuffer /* destination buffer */,
      0 /* destination offset */,
      4 /* size */
    );

    // Submit copy commands.
    const copyCommands = copyEncoder.finish();
    ok(copyCommands);
    device.queue.submit([copyCommands]);

    // Read buffer.
    await gpuReadBuffer.mapAsync(GPUMapMode.READ);
    const copyArrayBuffer = gpuReadBuffer.getMappedRange();
    ok(copyArrayBuffer);

    deepEqual(new Uint8Array(copyArrayBuffer), new Uint8Array([ 0, 1, 2, 3 ]));
  },
};
