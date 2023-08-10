import { deepEqual, ok } from "node:assert";

// run manually for now
// bazel run --//src/workerd/io:enable_experimental_webgpu //src/workerd/server:workerd -- test `realpath ./src/workerd/api/gpu/webgpu-compute-test.gpu-wd-test` --verbose --experimental

export const read_sync_stack = {
  async test(ctrl, env, ctx) {
    ok(navigator.gpu);
    if (!("gpu" in navigator)) {
      console.log(
        "WebGPU is not supported. Enable chrome://flags/#enable-unsafe-webgpu flag."
      );
      return;
    }

    const adapter = await navigator.gpu.requestAdapter();
    ok(adapter);

    const adapterInfo = await adapter.requestAdapterInfo(["device"]);
    ok(adapterInfo);
    ok(adapterInfo.device);

    ok(adapter.features.keys());
    ok(adapter.features.has("depth-clip-control"));

    ok(adapter.limits);
    ok(adapter.limits.maxBufferSize);

    const requiredFeatures = [];
    requiredFeatures.push("texture-compression-astc");
    requiredFeatures.push("depth-clip-control");
    const device = await adapter.requestDevice({
      requiredFeatures,
    });
    ok(device);

    ok(device.lost);

    const firstMatrix = new Float32Array([
      2 /* rows */, 4 /* columns */, 1, 2, 3, 4, 5, 6, 7, 8,
    ]);

    device.pushErrorScope("out-of-memory");
    device.pushErrorScope("validation");

    const gpuBufferFirstMatrix = device.createBuffer({
      mappedAtCreation: true,
      size: firstMatrix.byteLength,
      usage: GPUBufferUsage.STORAGE,
    });
    ok(gpuBufferFirstMatrix);

    ok((await device.popErrorScope()) === null);

    const arrayBufferFirstMatrix = gpuBufferFirstMatrix.getMappedRange();
    ok(arrayBufferFirstMatrix);

    new Float32Array(arrayBufferFirstMatrix).set(firstMatrix);
    gpuBufferFirstMatrix.unmap();

    // Second Matrix
    const secondMatrix = new Float32Array([
      4 /* rows */, 2 /* columns */, 1, 2, 3, 4, 5, 6, 7, 8,
    ]);

    const gpuBufferSecondMatrix = device.createBuffer({
      mappedAtCreation: true,
      size: secondMatrix.byteLength,
      usage: GPUBufferUsage.STORAGE,
    });
    ok(gpuBufferSecondMatrix);

    const arrayBufferSecondMatrix = gpuBufferSecondMatrix.getMappedRange();
    ok(arrayBufferSecondMatrix);

    new Float32Array(arrayBufferSecondMatrix).set(secondMatrix);
    gpuBufferSecondMatrix.unmap();

    // Result Matrix
    const resultMatrixBufferSize =
      Float32Array.BYTES_PER_ELEMENT * (2 + firstMatrix[0] * secondMatrix[1]);
    const resultMatrixBuffer = device.createBuffer({
      size: resultMatrixBufferSize,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC,
    });
    ok(resultMatrixBuffer);

    // Bind group layout and bind group
    const bindGroupLayout = device.createBindGroupLayout({
      entries: [
        {
          binding: 0,
          visibility: GPUShaderStage.COMPUTE,
          buffer: {
            type: "read-only-storage",
          },
        },
        {
          binding: 1,
          visibility: GPUShaderStage.COMPUTE,
          buffer: {
            type: "read-only-storage",
          },
        },
        {
          binding: 2,
          visibility: GPUShaderStage.COMPUTE,
          buffer: {
            type: "storage",
          },
        },
      ],
    });
    ok(bindGroupLayout);

    const bindGroup = device.createBindGroup({
      layout: bindGroupLayout,
      entries: [
        {
          binding: 0,
          resource: {
            buffer: gpuBufferFirstMatrix,
          },
        },
        {
          binding: 1,
          resource: {
            buffer: gpuBufferSecondMatrix,
          },
        },
        {
          binding: 2,
          resource: {
            buffer: resultMatrixBuffer,
          },
        },
      ],
    });
    ok(bindGroup);

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
        for (var i = 0u; i < u32(firstMatrix.size.y); i = i + 1u) {
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
    ok(compilationInfo.messages.length == 0);

    // Pipeline setup
    const computePipeline = device.createComputePipeline({
      layout: device.createPipelineLayout({
        bindGroupLayouts: [bindGroupLayout],
      }),
      compute: {
        module: shaderModule,
        entryPoint: "main",
      },
    });
    ok(computePipeline);

    // Pipeline with auto layout
    const computePipelineAuto = device.createComputePipeline({
      layout: "auto",
      compute: {
        module: shaderModule,
        entryPoint: "main",
      },
    });
    ok(computePipelineAuto);

    // bind group with inferred layout
    const bindGroupAutoLayout = device.createBindGroup({
      layout: computePipelineAuto.getBindGroupLayout(0 /* index */),
      entries: [
        {
          binding: 0,
          resource: {
            buffer: gpuBufferFirstMatrix,
          },
        },
        {
          binding: 1,
          resource: {
            buffer: gpuBufferSecondMatrix,
          },
        },
        {
          binding: 2,
          resource: {
            buffer: resultMatrixBuffer,
          },
        },
      ],
    });
    ok(bindGroupAutoLayout);

    // Commands submission
    const commandEncoder = device.createCommandEncoder();
    ok(commandEncoder);

    const passEncoder = commandEncoder.beginComputePass();
    ok(passEncoder);
    passEncoder.setPipeline(computePipeline);
    passEncoder.setBindGroup(0, bindGroup);

    const workgroupCountX = Math.ceil(firstMatrix[0] / 8);
    const workgroupCountY = Math.ceil(secondMatrix[1] / 8);
    passEncoder.dispatchWorkgroups(workgroupCountX, workgroupCountY);
    passEncoder.end();

    // Get a GPU buffer for reading in an unmapped state.
    const gpuReadBuffer = device.createBuffer({
      size: resultMatrixBufferSize,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    ok(gpuReadBuffer);

    // Encode commands for copying buffer to buffer.
    commandEncoder.copyBufferToBuffer(
      resultMatrixBuffer /* source buffer */,
      0 /* source offset */,
      gpuReadBuffer /* destination buffer */,
      0 /* destination offset */,
      resultMatrixBufferSize /* size */
    );

    // Submit GPU commands.
    const gpuCommands = commandEncoder.finish();
    ok(gpuCommands);

    device.queue.submit([gpuCommands]);

    // Read buffer.
    await gpuReadBuffer.mapAsync(GPUMapMode.READ);

    const arrayBuffer = gpuReadBuffer.getMappedRange();
    ok(arrayBuffer);

    deepEqual(
      new Float32Array(arrayBuffer),
      new Float32Array([2, 2, 50, 60, 114, 140])
    );
  },
};
