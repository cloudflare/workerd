
export default {
  async fetch(req) {
    return new Response("ok");
  },

  async connect(socket) {
    // pipe the input stream to the output
    socket.readable.pipeTo(socket.writable);
  }
};
