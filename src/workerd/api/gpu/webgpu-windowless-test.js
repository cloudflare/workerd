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

    const outputBufferSize = 4 * textureSize * textureSize;
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
