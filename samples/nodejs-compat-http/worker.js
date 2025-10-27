import { get } from 'node:http';
export default {
  async fetch(request) {
    const { promise, resolve } = Promise.withResolvers();
    get(
      'http://example.com',
      {
        headers: {
          accept: 'text/plain',
        },
      },
      (res) => {
        let data = '';
        res.on('data', (chunk) => {
          data += chunk;
        });
        res.on('end', () => {
          console.log('...', data);
          resolve(new Response(data));
        });
      }
    );
    return promise;
  },
};
