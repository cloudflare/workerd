let counter = 0;
const fr = new FinalizationRegistry(() => {
  ++counter;
});

export default {
  async fetch(request, env, ctx) {
    (function () {
      let obj = {};
      fr.register(obj, '');
      obj = undefined;
    })();

    // Ensure obj gets GC'd
    gc();

    return new Response(counter);
  },
};
