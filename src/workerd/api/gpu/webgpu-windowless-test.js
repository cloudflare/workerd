import { ok, deepEqual, equal } from "node:assert";

// run manually for now
// bazel run --//src/workerd/io:enable_experimental_webgpu //src/workerd/server:workerd -- test `realpath ./src/workerd/api/gpu/webgpu-windowless-test.gpu-wd-test` --verbose --experimental

async function hash(data) {
  const hashBuffer = await crypto.subtle.digest("SHA-256", data);
  const hashArray = Array.from(new Uint8Array(hashBuffer));
  const hashHex = hashArray
    .map((bytes) => bytes.toString(16).padStart(2, "0"))
    .join("");
  return hashHex;
}

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

    const textureSize = 256;
    const textureDesc = {
      size: { width: textureSize, height: textureSize, depthOrArrayLayers: 1 },
      format: "rgba8unorm-srgb",
      usage: GPUTextureUsage.COPY_SRC | GPUTextureUsage.RENDER_ATTACHMENT,
    };
    const texture = device.createTexture(textureDesc);
    ok(texture);

    const textureView = texture.createView();
    ok(textureView);

    const u32Size = 4;
    const outputBufferSize = u32Size * textureSize * textureSize;
    const outputBuffer = device.createBuffer({
      size: outputBufferSize,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    ok(outputBuffer);

    const shaderModule = device.createShaderModule({
      code: `
      // Vertex shader

      struct VertexOutput {
          @builtin(position) clip_position: vec4<f32>,
      };

      @vertex
      fn vs_main(
          @builtin(vertex_index) in_vertex_index: u32,
      ) -> VertexOutput {
          var out: VertexOutput;
          let x = f32(1 - i32(in_vertex_index)) * 0.5;
          let y = f32(i32(in_vertex_index & 1u) * 2 - 1) * 0.5;
          out.clip_position = vec4<f32>(x, y, 0.0, 1.0);
          return out;
      }

      // Fragment shader

      @fragment
      fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
          return vec4<f32>(0.3, 0.2, 0.1, 1.0);
      }
    `,
    });
    ok(shaderModule);

    const pipelineLayout = device.createPipelineLayout({
      bindGroupLayouts: [],
      pushConstantRanges: [],
    });
    ok(pipelineLayout);

    const renderPipeline = device.createRenderPipeline({
      layout: pipelineLayout,
      vertex: {
        module: shaderModule,
        entryPoint: "vs_main",
        buffers: [],
      },
      fragment: {
        module: shaderModule,
        entryPoint: "fs_main",
        targets: [
          {
            format: textureDesc.format,
            writeMask: GPUColorWrite.ALL,
            blend: {
              color: {},
              alpha: {},
            },
          },
        ],
      },
      primitive: {
        frontFace: "ccw",
        GPUCullMode: "back",
      },
      multisample: {
        count: 1,
        mask: 0xffffffff,
        alphaToCoverageEnabled: false,
      },
    });
    ok(renderPipeline);

    const encoder = device.createCommandEncoder();

    const renderPassDescriptor = {
      colorAttachments: [
        {
          clearValue: { r: 0.1, g: 0.2, b: 0.3, a: 1.0 },
          loadOp: "clear",
          storeOp: "store",
          view: textureView,
        },
      ],
    };
    const renderPass = encoder.beginRenderPass(renderPassDescriptor);
    ok(renderPass);

    renderPass.setPipeline(renderPipeline);
    renderPass.draw(3);
    renderPass.end();

    encoder.copyTextureToBuffer(
      {
        aspect: "all",
        texture: texture,
        mipLevel: 0,
        origin: {},
      },
      {
        buffer: outputBuffer,
        offset: 0,
        bytesPerRow: u32Size * textureSize,
        rowsPerImage: textureSize,
      },
      textureDesc.size
    );

    // Submit GPU commands.
    const gpuCommands = encoder.finish();
    device.queue.submit([gpuCommands]);

    await outputBuffer.mapAsync(GPUMapMode.READ);

    const data = outputBuffer.getMappedRange();
    ok(data);
    const result = await hash(data);
    equal(
      result,
      "dd7fd0917e7f9383fd7f2ae5027bf6a4f8f90b2ab5c69c52d4f29a856bd9165a"
    );
    outputBuffer.unmap();

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
