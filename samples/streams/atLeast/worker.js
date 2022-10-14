addEventListener('fetch', event => {
  event.respondWith(handle(event.request));
});

async function handle(request) {

  const chunks = [
    "never gonna give you up",
    "never gonna let you go",
    "boom",
  ];

  const enc = new TextEncoder();
  const dec = new TextDecoder();

  const rs = new ReadableStream({
    type: 'bytes',

    pull(c) {
      const byobRequest = c.byobRequest;
      if (chunks.length === 0) {
        // In this case, the previous read should have been short,
        // and the controller is expecting us to close things down.
        c.close();
        byobRequest.respond(0);
      } else {
        const { written } = enc.encodeInto(chunks.shift(), byobRequest.view);
        byobRequest.respond(written);
      }
    },
  });

  const min = 23;
  const ab = new ArrayBuffer(min);

  const reader = rs.getReader({ mode: 'byob' });

  let res = await reader.readAtLeast(min, new Uint8Array(ab));

  console.log(dec.decode(res.value));

  res = await reader.readAtLeast(min, new Uint8Array(res.value.buffer));

  console.log(dec.decode(res.value));

  res = await reader.readAtLeast(min, new Uint8Array(res.value.buffer));

  console.log(res.done);

  return new Response("OK");
}
