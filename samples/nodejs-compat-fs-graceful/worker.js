import { default as fs } from 'graceful-fs';

export default {
  async fetch(request) {
    return new Response(fs.readFileSync('/bundle/worker'));
  }
};
