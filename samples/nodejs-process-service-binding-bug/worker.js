export default {
  async fetch(request) {
    return new Response(process.env.SERVICE_BINDING);
  },
};
