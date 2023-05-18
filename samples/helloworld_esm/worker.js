let workerCount = 0;

export default {
  async fetch(_req, env) {
    const esModule = `
    console.log('hello from inner js')
    export default {
      async task() {
        return 'this is a CID';
      }
    }
    `
    const workerConfig = JSON.stringify({
      services: [{
        name: `inner-main-${++workerCount}`,
        worker: {
          modules: [
            {name: 'worker', esModule },
          ],
          compatibilityDate: '2023-02-28',
        }
      }],
    });
    return new Response(`${await env.workerd.runWorker(workerConfig)}\n`);
  }
};
