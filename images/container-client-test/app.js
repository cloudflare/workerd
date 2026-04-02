const { createServer } = require('http');

const webSocketEnabled = process.env.WS_ENABLED === 'true';
const wsProxyTarget = process.env.WS_PROXY_TARGET || null;
const wsProxySecure = process.env.WS_PROXY_SECURE === 'true';

const server = createServer(function (req, res) {
  if (req.url === '/ws') {
    return;
  }

  if (req.url === '/pid-namespace') {
    // Return the PID namespace inode. When running in an isolated PID namespace,
    // this will differ from the host's PID namespace. We return the inode so the
    // test can verify isolation by comparing against a known value or checking
    // that PID 1 in this namespace is NOT the host's init process.
    const fs = require('fs');
    try {
      // Read /proc/1/cmdline to see what process is PID 1 in this namespace.
      // In an isolated namespace, PID 1 will be our container's init process.
      // In host namespace, PID 1 will be the host's init (e.g., systemd, launchd).
      const init = fs.readFileSync('/proc/1/cmdline', 'utf8');
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.write(
        JSON.stringify({
          pid: process.pid,
          ppid: process.ppid,
          init: init.replace(/\0/g, ' ').trim(),
        })
      );
      res.end();
    } catch (err) {
      res.writeHead(500, { 'Content-Type': 'text/plain' });
      res.write(`Error reading /proc/1/cmdline: ${err.message}`);
      res.end();
    }

    return;
  }

  // Write a file inside the container (for snapshot testing)
  if (req.url.startsWith('/write-file')) {
    const url = new URL(req.url, 'http://localhost');
    const filePath = url.searchParams.get('path');
    if (!filePath) {
      res.writeHead(400, { 'Content-Type': 'text/plain' });
      res.write('Missing "path" query param');
      res.end();
      return;
    }
    const fs = require('fs');
    const path = require('path');
    let body = '';
    req.on('data', (chunk) => (body += chunk));
    req.on('end', () => {
      try {
        fs.mkdirSync(path.dirname(filePath), { recursive: true });
        fs.writeFileSync(filePath, body);
        res.writeHead(200, { 'Content-Type': 'text/plain' });
        res.write('ok');
        res.end();
      } catch (err) {
        res.writeHead(500, { 'Content-Type': 'text/plain' });
        res.write(err.message);
        res.end();
      }
    });
    return;
  }

  // Read a file inside the container (for snapshot testing)
  if (req.url.startsWith('/read-file')) {
    const url = new URL(req.url, 'http://localhost');
    const filePath = url.searchParams.get('path');
    if (!filePath) {
      res.writeHead(400, { 'Content-Type': 'text/plain' });
      res.write('Missing "path" query param');
      res.end();
      return;
    }
    const fs = require('fs');
    try {
      const content = fs.readFileSync(filePath, 'utf8');
      res.writeHead(200, { 'Content-Type': 'text/plain' });
      res.write(content);
      res.end();
    } catch (err) {
      res.writeHead(404, { 'Content-Type': 'text/plain' });
      res.write(err.message);
      res.end();
    }
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

  if (req.url === '/intercept-https') {
    const targetHost = req.headers['x-host'] || 'example.com';
    fetch(`https://${targetHost}`)
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
      const protocol = wsProxySecure ? 'wss' : 'ws';
      const targetWs = new WebSocket(`${protocol}://${wsProxyTarget}/ws`);
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
