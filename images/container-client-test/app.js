const { createServer } = require('http');

const webSocketEnabled = process.env.WS_ENABLED === 'true';

// Create HTTP server
const server = createServer(function (req, res) {
  if (req.url === '/ws') {
    // WebSocket upgrade will be handled by the WebSocket server
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
        res.write(err.message);
        res.end();
      });
    return;
  }

  res.writeHead(200, { 'Content-Type': 'text/plain' });
  res.write('Hello World!');
  res.end();
});

// Check if WebSocket functionality is enabled
if (webSocketEnabled) {
  const WebSocket = require('ws');

  // Create WebSocket server
  const wss = new WebSocket.Server({
    server: server,
    path: '/ws',
  });

  wss.on('connection', function connection(ws) {
    console.log('WebSocket connection established');

    ws.on('message', function message(data) {
      console.log('Received:', data.toString());
      // Echo the message back with prefix
      ws.send('Echo: ' + data.toString());
    });

    ws.on('close', function close() {
      console.log('WebSocket connection closed');
    });

    ws.on('error', console.error);
  });
}

server.listen(8080, function () {
  console.log('Server listening on port 8080');
  if (webSocketEnabled) {
    console.log('WebSocket support enabled');
  }
});
