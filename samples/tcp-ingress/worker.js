
export default {
  async fetch(req) {
    return new Response("ok");
  },

  connect({inbound, cf}) {
    console.log(cf);
    return inbound.pipeThrough(new IdentityTransformStream());
  }
};
