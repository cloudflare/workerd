// This is a simple SSE server written for Node.js ... it sends a message every second
// for 10 seconds, then disconnects. The worker.js file will use the EventSource API to
// connect to this server.

const { createServer } = require('http');

let counter = 0;

function getMessage(txt) {
  return `data: ${txt}\nid: ${counter++}\n\n`;
}

const server = createServer((req, res) => {
  res.writeHead(200, {
    'Content-Type': 'text/event-stream',
    'Cache-Control': 'no-cache',
    'Connection': 'keep-alive'
  });

  res.write(getMessage('Hello World'));

  let t = setInterval(() => {
    res.write(getMessage('Hello World'));
    if (counter === 10) {
      clearInterval(t);
      res.end();
      counter = 0;
    }
  }, 1000);
});

server.listen(8888, () => {
  console.log('Server is running...');
});
