import { ok } from "node:assert";

// run manually for now
// bazel run --//src/workerd/io:enable_experimental_webgpu //src/workerd/server:workerd -- test `realpath ./src/workerd/api/gpu/webgpu-errors-test.gpu-wd-test` --verbose --experimental

export const read_sync_stack = {
  async test(ctrl, env, ctx) {
    ok(navigator.gpu);

    const adapter = await navigator.gpu.requestAdapter();
    ok(adapter);
    ok(adapter instanceof GPUAdapter);

    const device = await adapter.requestDevice();
    ok(device);

    let callbackCalled = false;
    device.addEventListener("uncapturederror", (event) => {
      ok(event.error.message.includes("9007199254740991"));
      callbackCalled = true;
    });

    device.createBuffer({
      mappedAtCreation: true,
      size: Number.MAX_SAFE_INTEGER,
      usage: GPUBufferUsage.STORAGE,
    });

    device.pushErrorScope("validation");

    device.createBuffer({
      mappedAtCreation: true,
      size: Number.MAX_SAFE_INTEGER,
      usage: GPUBufferUsage.STORAGE,
    });

    const error = await device.popErrorScope();
    ok(error);
    ok(error.message);

    // Compute shader code
    const shaderModule = device.createShaderModule({
      code: `
      struct Matrix {
        size : vec2<f32>,
        numbers: array<f32>,
      }

      @group(0) @binding(0) var<storage, read> firstMatrix : Matrix;
      @group(0) @binding(1) var<storage, read> secondMatrix : Matrix;
      @group(0) @binding(2) var<storage, read_write> resultMatrix : Matrix;

      @compute @workgroup_size(8, 8)
      fn main(@builtin(global_invocation_id) global_id : vec3<u32>) {
        // Guard against out-of-bounds work group sizes
        if (global_id.x >= u32(firstMatrix.size.x) || global_id.y >= u32(secondMatrix.size.y)) {
          return;
        }

        resultMatrix.size = vec2(firstMatrix.size.x, secondMatrix.size.y);

        let resultCell = vec2(global_id.x, global_id.y);
        var result = 0.0;
        for var i = 0u; i < u32(firstMatrix.size.y); i = i + 1u) {
          let a = i + resultCell.x * u32(firstMatrix.size.y);
          let b = resultCell.y + i * u32(secondMatrix.size.y);
          result = result + firstMatrix.numbers[a] * secondMatrix.numbers[b];
        }

        let index = resultCell.y + resultCell.x * u32(secondMatrix.size.y);
        resultMatrix.numbers[index] = result;
      }
    `,
    });
    ok(shaderModule);

    const compilationInfo = await shaderModule.getCompilationInfo();
    ok(compilationInfo.messages.length === 1);
    ok(compilationInfo.messages[0].type === "error");

    // ensure callback with error was indeed called
    ok(callbackCalled);
  },
};
