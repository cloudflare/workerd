let frCounter = 0;
let weakRefState = { isDereferenced: false, value: null };

const fr = new FinalizationRegistry(() => {
  ++frCounter;
});

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const test = url.searchParams.get('test') || 'fr';

    if (test === 'fr') {
      // Test FinalizationRegistry
      (function () {
        fr.register({}, '');
      })();

      // Ensure obj gets GC'd
      gc();

      if (frCounter > 0) {
        await scheduler.wait(10);
      }

      return new Response(frCounter.toString());
    } else if (test === 'weakref') {
      // Test WeakRef
      if (url.searchParams.has('create')) {
        const obj = { message: "I'm alive!" };
        const ref = new WeakRef(obj);

        // Store the WeakRef for later testing
        weakRefState.ref = ref;
        weakRefState.isDereferenced = false;
        weakRefState.value = ref.deref()?.message || null;

        return Response.json({
          created: true,
          value: weakRefState.value,
        });
      }
      if (url.searchParams.has('gc')) {
        // Force garbage collection and check if the WeakRef has been cleared
        gc();
        await scheduler.wait(10); // Give GC time to clean up
      }

      // Just return the current state
      const value = weakRefState.ref?.deref()?.message || null;
      weakRefState.isDereferenced = value === null;
      weakRefState.value = value;

      return Response.json({
        isDereferenced: weakRefState.isDereferenced,
        value: weakRefState.value,
      });
    }

    return new Response('Invalid test', { status: 400 });
  },
};
