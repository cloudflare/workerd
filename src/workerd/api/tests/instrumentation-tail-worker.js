// tailStream is going to be invoked multiple times, but we want to wait
// to run the test until all executions are done. Collect promises for
// each
export let spans = new Map();
export let invocationPromises = [];

// tail stream handler function used in several STW instrumentation tests.
export const testTailHandler = {
  tailStream(event, env, ctx) {
    // Capture the top-level span ID from the onset event
    const topLevelSpanId = event.event.spanId;

    // For each "onset" event, store a promise which we will resolve when
    // we receive the equivalent "outcome" event
    let resolveFn;
    invocationPromises.push(
      new Promise((resolve, reject) => {
        resolveFn = resolve;
      })
    );

    return (event) => {
      // For spanOpen events, the new span ID is in event.event.spanId
      // For other events, they reference an existing span via event.spanContext.spanId
      let spanKey = event.invocationId + event.spanContext.spanId;
      switch (event.event.type) {
        case 'spanOpen':
          // spanOpen creates a new span with ID in event.event.spanId
          spanKey = event.invocationId + event.event.spanId;
          spans.set(spanKey, {
            name: event.event.name,
          });
          break;
        case 'attributes': {
          // Filter out top-level attributes events (jsRpcSession span)
          if (event.spanContext.spanId === topLevelSpanId) {
            // Ignore attributes for the top-level span
            break;
          }

          // attributes references an existing span via spanContext.spanId
          let span = spans.get(spanKey);
          if (!span) {
            throw new Error(`Attributes event for unknown span: ${spanKey}`);
          }
          for (let { name, value } of event.event.info) {
            span[name] = value;
          }
          break;
        }
        case 'spanClose': {
          // spanClose references an existing span via spanContext.spanId
          let span = spans.get(spanKey);
          if (!span) {
            throw new Error(`SpanClose event for unknown span: ${spanKey}`);
          }
          span['closed'] = true;
          break;
        }
        case 'outcome':
          resolveFn();
          break;
      }
    };
  },
};
