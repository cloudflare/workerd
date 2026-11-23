// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Uploads the fixture payloads with the AWS SDK for JavaScript v3.
import { S3Client, PutObjectCommand } from '@aws-sdk/client-s3';
import { NodeHttpHandler } from '@smithy/node-http-handler';
import { Readable } from 'node:stream';
import https from 'node:https';

const SENTENCE = 'The quick brown fox jumps over the lazy dog. ';
const payload = (n) =>
  Buffer.from(SENTENCE.repeat(Math.ceil(n / SENTENCE.length) + 1).slice(0, n));
const [httpPort, httpsPort] = process.argv.slice(2);

function client(secure) {
  return new S3Client({
    region: 'us-east-1',
    endpoint: secure
      ? `https://127.0.0.1:${httpsPort}`
      : `http://127.0.0.1:${httpPort}`,
    forcePathStyle: true,
    credentials: {
      accessKeyId: 'AKIDEXAMPLE',
      secretAccessKey: 'wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY',
    },
    requestHandler: new NodeHttpHandler({
      httpsAgent: new https.Agent({ rejectUnauthorized: false }),
    }),
  });
}

// Writes the body as several stream chunks, as a request body would arrive.
function stream(data, pieces) {
  const size = Math.ceil(data.length / pieces) || 1;
  const chunks = [];
  for (let i = 0; i < data.length; i += size)
    chunks.push(data.subarray(i, i + size));
  return Readable.from(chunks);
}

async function put(name, size, { secure = true, pieces = 1, ...options } = {}) {
  const data = payload(size);
  await client(secure).send(
    new PutObjectCommand({
      Bucket: 'fixtures',
      Key: name,
      Body: stream(data, pieces),
      ContentLength: size,
      ...options,
    })
  );
}

await put('js-v3-crc32', 70000, { pieces: 3 });
await put('js-v3-crc32-http', 1000, { secure: false, pieces: 2 });
await put('js-v3-sha256', 1000, { ChecksumAlgorithm: 'SHA256' });
await put('js-v3-crc64nvme', 1000, { ChecksumAlgorithm: 'CRC64NVME' });
await put('js-v3-empty', 0);
