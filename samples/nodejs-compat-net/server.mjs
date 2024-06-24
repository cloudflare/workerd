import { createServer } from 'node:net';

const server = createServer((socket) => {
  console.log('Connected...');
  socket.setEncoding('utf8');
  socket.on('data', console.log);
  const i = setInterval(() => {
    socket.write('ping');
  }, 1000);
  socket.once('close', () => {
    console.log('Disconnected...');
    clearInterval(i);
  });
  socket.on('error', (err) => {
    console.log('socket errored', err);
  });
});

server.listen(8888, () => {
  console.log('Listening...');
});
