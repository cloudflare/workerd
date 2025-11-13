export default {
  async test(ctrl, env, ctx) {
    const resp = await env.PYTHON.fetch('http://example.com/websocket', {
      headers: { Upgrade: 'websocket' },
    });
    const ws = resp.webSocket;
    if (!ws) {
      throw new Error('No websocket');
    }
    ws.accept();
    const messagePromise = new Promise((resolve) => {
      ws.addEventListener('message', (msg) => {
        console.log('Received in JS Tester: ', msg.data);
        resolve(msg.data);
      });
    });
    ws.send('test');

    await messagePromise;
    ws.close();
  },
};
