export default {
  async fetch(req, env) {
    console.log('hello to the tail worker!');
    return new Response("Hello World\n");
  }
};
