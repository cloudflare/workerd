#!/usr/bin/env node

const { createServer } = require('http');

const server = createServer((req, res) => {
  // The response from the fallback service must be a valid JSON
  // serialization of a Worker::Module config.

  // The x-resolve-method tells us if the module was imported or required.
  console.log(req.headers['x-resolve-method']);

  // The req.url query params tell us what we are importing
  const url = new URL(req.url, "http://example.org");
  const specifier = url.searchParams.get('specifier');
  const referrer = url.searchParams.get('referrer');
  console.log(specifier, referrer);

  // The fallback service can tell the client to map the request
  // specifier to another specifier using a 301 redirect, using
  // the location header to specify the alternative specifier.
  if (specifier == "/foo") {
    console.log('Redirecting /foo to /baz');
    res.writeHead(301, { location: '/baz' });
    res.end();
    return;
  }

  if (specifier == "/bar") {
    res.writeHead(404);
    res.end();
    return;
  }

  console.log(`Returning module spec for ${specifier}`);
  // Returning the name is optional. If it is included, then it MUST match the
  // request specifier!
  res.end(`{
  "name": "${specifier}",
  "esModule":"export default 1;"
}`);
});

server.listen(8888, () => {
  console.log('ready...');
});
