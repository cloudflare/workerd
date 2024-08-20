import { deepEqual, ok, equal } from 'node:assert';

export class DurableObjectExample {
  constructor(state) {
    this.state = state;
  }

  async fetch() {
    ok(navigator.gpu);
    const adapter = await navigator.gpu.requestAdapter();
    ok(adapter);
    const device = await adapter.requestDevice();
    ok(device);

    // Get a GPU buffer in a mapped state and an arrayBuffer for writing.
    const bufferSize = 16384;
    const bufferContents = Array.from(
      { length: bufferSize },
      (_, index) => index
    );
    const emptyBufferContents = Array(bufferSize).fill(0);
    const gpuWriteBuffer = device.createBuffer({
      label: 'gpuWriteBuffer',
      mappedAtCreation: true,
      size: bufferSize,
      usage: GPUBufferUsage.MAP_WRITE | GPUBufferUsage.COPY_SRC,
    });
    ok(gpuWriteBuffer);

    deepEqual(gpuWriteBuffer.size, BigInt(bufferSize));

    deepEqual(gpuWriteBuffer.usage, 6);

    deepEqual(gpuWriteBuffer.mapState, 'mapped');

    const arrayBuffer = gpuWriteBuffer.getMappedRange();
    ok(arrayBuffer);

    // Write bytes to buffer.
    new Uint8Array(arrayBuffer).set(bufferContents);

    // Unmap buffer so that it can be used later for copy.
    gpuWriteBuffer.unmap();

    // Get a GPU buffer for reading in an unmapped state.
    const gpuReadBuffer = device.createBuffer({
      label: 'gpuReadBuffer',
      mappedAtCreation: false,
      size: bufferSize,
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
      bufferSize
    );

    // Submit copy commands.
    const copyCommands = copyEncoder.finish();
    ok(copyCommands);
    device.queue.submit([copyCommands]);

    // Read buffer.
    await gpuReadBuffer.mapAsync(GPUMapMode.READ);
    const copyArrayBuffer = gpuReadBuffer.getMappedRange();
    ok(copyArrayBuffer);

    deepEqual(new Uint8Array(copyArrayBuffer), new Uint8Array(bufferContents));
    gpuReadBuffer.unmap();

    // Clear buffer
    const clearEncoder = device.createCommandEncoder();
    clearEncoder.clearBuffer(gpuReadBuffer);
    const clearCommands = clearEncoder.finish();
    ok(clearCommands);
    device.queue.submit([clearCommands]);

    // Read cleared buffer.
    await gpuReadBuffer.mapAsync(GPUMapMode.READ);
    const copyClearedArrayBuffer = gpuReadBuffer.getMappedRange();
    ok(copyClearedArrayBuffer);

    deepEqual(
      new Uint8Array(copyClearedArrayBuffer),
      new Uint8Array(emptyBufferContents)
    );
    gpuReadBuffer.unmap();

    const textureSize = 64;
    const textureDesc = {
      size: { width: textureSize, height: textureSize, depthOrArrayLayers: 1 },
      format: 'rgba8unorm-srgb',
      usage: GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST,
    };
    const texture = device.createTexture(textureDesc);
    const copyTexture = device.createTexture(textureDesc);

    // Copy textures
    const u32Size = 4;
    const textureEncoder = device.createCommandEncoder();
    textureEncoder.copyBufferToTexture(
      {
        buffer: gpuWriteBuffer,
        bytesPerRow: u32Size * textureSize,
        rowsPerImage: textureSize,
      },
      { texture: texture },
      textureDesc.size
    );
    textureEncoder.copyTextureToTexture(
      { texture: texture },
      { texture: copyTexture },
      textureDesc.size
    );
    textureEncoder.copyTextureToBuffer(
      { texture: copyTexture },
      {
        buffer: gpuReadBuffer,
        bytesPerRow: u32Size * textureSize,
        rowsPerImage: textureSize,
      },
      textureDesc.size
    );
    const textureCommands = textureEncoder.finish();
    device.queue.submit([textureCommands]);

    // Read cleared buffer.
    await gpuReadBuffer.mapAsync(GPUMapMode.READ);
    const copyTextureArrayBuffer = gpuReadBuffer.getMappedRange();

    deepEqual(
      new Uint8Array(copyTextureArrayBuffer),
      new Uint8Array(bufferContents)
    );
    gpuReadBuffer.unmap();

    device.queue.writeBuffer(
      gpuReadBuffer,
      0,
      new Uint16Array([0, 1, 2, 3]),
      0,
      4
    );

    return new Response('OK');
  }
}

export const buffer_mapping = {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName('A');
    let obj = env.ns.get(id);
    let res = await obj.fetch('http://foo/test');
    let text = await res.text();
    equal(text, 'OK');
  },
};
