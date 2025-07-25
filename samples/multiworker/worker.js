
export default {
  async fetch(request, env, ctx) {

    let ret = {}

    // Promise pipelining with primitives
		ret.pipelining = await env.GATEWAY.promisePipeline().bar.baz();

    // // Returning a callable function
    using f = await env.GATEWAY.newFnCounter();
		await f(2);
		await f(1);
		ret.function = await f(-2);

    // // Sending a function argument
    ret.functionArg = await env.GATEWAY.apply((x) => x + 2, 3);

    // // // Returning an RpcTarget
    const counter = await env.GATEWAY.newCounter();
		await counter.increment(1);
		await counter.increment(-5);
		ret.rpcTarget = await counter.value()

    // // Returning a pipelined RpcTarget
    ret.rpcTargetPipeline = await env.GATEWAY.newCounter().increment(3);

		return Response.json(ret);
  }
}
