addEventListener('fetch', event => {
  event.respondWith(handle(event.request));
});

async function handle(request) {
  return new Response("Hello World\n");
}
