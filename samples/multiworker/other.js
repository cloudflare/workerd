import {WorkerEntrypoint, RpcTarget} from "cloudflare:workers"
import {receiveRpcOverHttp} from "rpc.js"

class Counter extends RpcTarget {
	#value = 0;

	increment(amount) {
		this.#value += amount;
		return this.#value;
	}

	value() {
		return this.#value;
	}
}

export class W extends WorkerEntrypoint {
  sayHello(){
    return "Hello there from other.js"
  }
  async newCounter() {
    return new Counter()
	}
	async newFnCounter() {
		let value = 0;
		return (increment = 0) => {
			value += increment;
			return value;
		};
	}
	async apply(fn, val) {
		return fn(val)
	}
	async promisePipeline() {
		return {
			bar: {
				baz: () => "qux",
			},
		};
	}
	async fetch() {
		return new Response("Hello World");
	}
}

export default {
  fetch(request, env) {
    console.log("Receiving", env.SELF)
    return receiveRpcOverHttp(request, env.SELF);
  }
}
