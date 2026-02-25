
export default {
  async fetch(req) {
    return new Response("ok");
  },

  async connect({socket, cf}) {
    console.log(cf);
    // pipe the input stream to the output
    socket.readable.pipeTo(socket.writable);
  }
};
