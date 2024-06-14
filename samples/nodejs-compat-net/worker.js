import * as net from 'node:net';

export default {
  async fetch(request) {
    const s = net.connect({
      host: '::1',
      port: 8888,
    }, () => {
      console.log('connected to server', s.remoteAddress, s.remotePort, s.remoteFamily);
    });

    s.setEncoding('utf8');

    s.write('pong1', (err) => console.log('written 1', err));
    s.write('pong2', (err) => console.log('written 2', err));
    s.write('pong3', (err) => console.log('written 3', err));
    s.write('pong4', (err) => console.log('written 4', err));
    s.write('pong5', (err) => console.log('written 5', err));
    s.write('pong6', (err) => console.log('written 6', err));
    s.write('pong7', (err) => console.log('written 7', err));
    s.write('pong8', (err) => console.log('written 8', err));

    s.on('data', console.log);

    s.once('ready', () => console.log('connection ready'));
    s.once('close', (hadError) => console.log('connection closed', hadError));
    s.once('error', (err) => console.log('connection error', err));

    await scheduler.wait(3 * 1000);

    console.log('ending connection');
    s.end();
    await scheduler.wait(3 * 1000);

    console.log(s.bytesRead, s.bytesWritten);

    return new Response("ok");
  }
};
