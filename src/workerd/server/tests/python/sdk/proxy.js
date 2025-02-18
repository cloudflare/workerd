export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (
      url.hostname == 'python-packages.edgeworker.net' ||
      url.hostname == 'pyodide-packages.runtime-playground.workers.dev'
    ) {
      return env.INTERNET.fetch(req);
    } else if (url.hostname == 'example.com') {
      return env.PYTHON.fetch(req);
    }

    throw new Error('Invalid url in proxy.js: ' + url);
  },
};
