// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import net from 'node:net';

const server = net.createServer((socket) => {
  let buffer = Buffer.alloc(0);
  let pendingCount = null;

  socket.on('data', (data) => {
    buffer = Buffer.concat([buffer, data]);

    while (true) {
      if (pendingCount !== null) {
        if (buffer.length < pendingCount) {
          return;
        }

        buffer = buffer.subarray(pendingCount);
        socket.write(`COUNTED ${pendingCount}\n`);
        socket.end();
        return;
      }

      const newline = buffer.indexOf(0x0a);
      if (newline === -1) {
        return;
      }

      const line = buffer.subarray(0, newline).toString('utf8');
      buffer = buffer.subarray(newline + 1);

      if (line.startsWith('BASIC ')) {
        socket.write(`echo: ${line.slice('BASIC '.length)}\n`);
        socket.end();
        return;
      }

      if (line.startsWith('ECHO ')) {
        socket.write(`${line.slice('ECHO '.length)}\n`);
        continue;
      }

      if (line === 'BYE') {
        socket.end();
        return;
      }

      if (line === 'FINAL') {
        socket.write('final message\n');
        socket.end();
        return;
      }

      if (line === 'LINES') {
        socket.write('line1\nline2\nline3\n');
        socket.end();
        return;
      }

      if (line.startsWith('COUNT ')) {
        pendingCount = Number(line.slice('COUNT '.length));
        continue;
      }

      if (line.startsWith('SEND ')) {
        const size = Number(line.slice('SEND '.length));
        socket.write(Buffer.alloc(size, 'y'));
        socket.end();
        return;
      }

      socket.destroy(new Error(`unknown command: ${line}`));
      return;
    }
  });
});

server.listen(0, process.env.SIDECAR_HOSTNAME, () => {
  console.log(`PYTHON_SOCKET_SERVER_PORT=${server.address().port}`);
});
