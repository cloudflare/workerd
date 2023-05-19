export default {
  async fetch(_req, env) {
    const esModule = `
    console.log('hello from inner js')
    // export default {
    //   async fetch() {
    //     return 'this is a CID';
    //   },
    //   async task() {
    //     return 'this is a CID';
    //   }
    // }
    `;
    // const workerConfig = JSON.stringify({
    //   services: [
    //     {
    //       name: `inner-main-${++workerCount}`,
    //       worker: {
    //         modules: [{ name: "worker", esModule }],
    //         compatibilityDate: "2023-02-28",
    //       },
    //     },
    //   ],
    // });
    const worker = new Worker(stringToDataUrl(esModule));
    return new Response("ok");
  },
};

function stringToDataUrl(str, mimeType = "text/plain") {
  const base64String = btoa(str);
  return `data:${mimeType};base64,${base64String}`;
}
