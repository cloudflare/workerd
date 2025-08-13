// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This works only in transpile mode
// enum From { World, TypeScript }

export default {
	async fetch(request, env, ctx): Promise<Response> {
		return new Response(`Hello World from TypeScript!`);

		// This works only in transpile mode
		// return new Response(`Hello World from ${From.TypeScript}!`);
	},
} satisfies ExportedHandler<Env>;
