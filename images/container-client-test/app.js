const { createServer } = require('http');

const webSocketEnabled = process.env.WS_ENABLED === 'true';
const wsProxyTarget = process.env.WS_PROXY_TARGET || null;

const server = createServer(function (req, res) {
  if (req.url === '/ws') {
    return;
  }

  if (req.url === '/intercept') {
    const targetHost = req.headers['x-host'] || '11.0.0.1';
    fetch(`http://${targetHost}`)
      .then((result) => result.text())
      .then((body) => {
        res.writeHead(200);
        res.write(body);
        res.end();
      })
      .catch((err) => {
        res.writeHead(500);
        res.write(`${targetHost} ${err.message}`);
        res.end();
      });
    return;
  }

  res.writeHead(200, { 'Content-Type': 'text/plain' });
  res.write('Hello World!');
  res.end();
});

if (webSocketEnabled) {
  const WebSocket = require('ws');
  const wss = new WebSocket.Server({ server, path: '/ws' });

  wss.on('connection', function (clientWs) {
    if (wsProxyTarget) {
      const targetWs = new WebSocket(`ws://${wsProxyTarget}/ws`);
      const ready = new Promise(function (resolve) {
        targetWs.on('open', resolve);
      });

      targetWs.on('message', (data) => clientWs.send(data));
      clientWs.on('message', async function (data) {
        await ready;
        targetWs.send(data);
      });

      clientWs.on('close', targetWs.close);
      targetWs.on('close', clientWs.close);
    } else {
      clientWs.on('message', function (data) {
        clientWs.send('Echo: ' + data.toString());
      });
    }
  });
}

server.listen(8080, function () {
  console.log('Server listening on port 8080');
  if (webSocketEnabled) {
    console.log('WebSocket support enabled');
  }
});
