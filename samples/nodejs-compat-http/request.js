import http from 'node:http';
import {Buffer} from 'node:buffer';

export default {
  async fetch(_req, _env) {
    const postData = JSON.stringify({
      'msg': 'Hello World!',
    });

    const options = {
      hostname: 'httpbin.org',
      port: 80,
      path: '/post',
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Content-Length': Buffer.byteLength(postData),
      },
    };

    const request = http.request(options);
    const responsePromise = new Promise((resolve) => {
      request.on('response', (resp) => resolve(resp));
    });
    request.write(postData);
    request.end();

    const res = await responsePromise;

    let rawData = '';
    res.on('data', (chunk) => { rawData += chunk; });

    const dataPromise = new Promise((resolve) => {
      res.on('end', () => {
        resolve(rawData);
      });
    })

    const data = await dataPromise;

    return new Response(`Response (${res.statusCode}): ${data}`);
  }
};
