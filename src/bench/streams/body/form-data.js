// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// formData() parsing multipart bodies: from a runtime-produced body, from a
// JavaScript stream delivering the body in chunks, and a FormData
// serialized into a body and parsed back.

import { chunkedValueStream, expectEqual } from 'helpers';

function formOf(fields, fileSize) {
  const form = new FormData();
  for (let i = 0; i < fields; i++) form.append(`field${i}`, `value ${i}`);
  if (fileSize > 0) {
    form.append('file', new File([new Uint8Array(fileSize)], 'file.bin'));
  }
  return form;
}

async function parse(body, contentType, entries) {
  const form = await new Response(body, {
    headers: { 'content-type': contentType },
  }).formData();
  expectEqual([...form.keys()].length, entries, 'entries parsed');
}

export const formData = {
  async bench(b) {
    for (const [fields, fileSize] of [
      [16, 0],
      [256, 0],
      [16, 262144],
    ]) {
      const serialized = new Response(formOf(fields, fileSize));
      const contentType = serialized.headers.get('content-type');
      const bytes = new Uint8Array(await serialized.arrayBuffer());
      const entries = fields + (fileSize > 0 ? 1 : 0);
      const label =
        `${fields} fields` +
        (fileSize > 0 ? ` and a ${fileSize / 1024} KiB file` : '');
      b.run(`parse ${label}`, () => parse(bytes, contentType, entries));
      b.run(`parse ${label}, from a value stream in 4 KiB chunks`, () =>
        parse(chunkedValueStream(bytes, 4096), contentType, entries)
      );
    }
    const form = formOf(16, 0);
    b.run('serialize 16 fields into a body and parse them', async () => {
      const parsed = await new Response(form).formData();
      expectEqual([...parsed.keys()].length, 16, 'entries parsed');
    });
  },
};
