import { env } from 'node:process';
import { createSocket } from 'node:dgram';
import { once } from 'node:events';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { WorkerdServerHarness } from '../server-harness.mjs';

for (const path of ['/worker', '/object', '/echo', '/denied']) {
  test(`outbound UDP ${path}`, { timeout: 30000 }, async (t) => {
    const collector = createSocket('udp4');
    const packets = [];
    collector.on('message', (packet, peer) => {
      packets.push(Buffer.from(packet));
      if (path === '/echo') collector.send(packet, peer.port, peer.address);
    });
    collector.bind(0, '127.0.0.1');
    await once(collector, 'listening');
    t.after(() => collector.close());
    const server = new WorkerdServerHarness({
      workerdBinary: env.WORKERD_BINARY,
      workerdConfig: env.WORKERD_CONFIG,
      listenPortNames: ['http'],
      extraArgs: ['--experimental'],
    });
    t.after(() => server.stop());
    await server.start();
    const port = await server.getListenPort('http');
    const host = path === '/denied' ? '&host=192.0.2.1' : '';
    const response = await fetch(
      `http://127.0.0.1:${port}${path}?port=${collector.address().port}${host}`
    );
    assert.equal(response.status, 200);
    assert.equal(await response.text(), path === '/denied' ? 'denied' : 'sent');
    if (path === '/denied') {
      assert.equal(packets.length, 0);
    } else if (path === '/echo') {
      assert.deepEqual(packets, [Buffer.from([0, 32, 255])]);
    } else {
      // Receipt at the independent collector is the assertion; write completion alone is not.
      const expected = [
        Buffer.alloc(0),
        Buffer.from([0, 32, 255]),
        Buffer.from([1, 2, 3]),
        ...[1, 1, 2, 2, 4, 4, 8, 8, 4, 8].map((width) =>
          Buffer.alloc(2 * width, 42)
        ),
        Buffer.alloc(3, 42),
        Buffer.alloc(9000, 42),
      ];
      while (packets.length < expected.length) {
        await once(collector, 'message', { signal: AbortSignal.timeout(5000) });
      }
      assert.deepEqual(packets, expected);
    }
  });
}
