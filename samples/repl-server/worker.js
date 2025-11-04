// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { default as UnsafeEval } from "workerd:unsafe-eval";
import { default as util } from "node:util";

// https://stackoverflow.com/questions/67322922/context-preserving-eval
var __EVAL = s => UnsafeEval.eval(`void (__EVAL = ${__EVAL.toString()}); ${s}`);

async function evaluate(expr) {
  try {
    const result = await __EVAL(expr);
    return result;
  } catch (err) {
    return err;
  }
}

globalThis.consoleLogBuf = "";

export default {
  async fetch(req, env) {
    const {cmd} = await req.json();

    globalThis.env = env;

    globalThis.console.log = (...args) => {
      const line = args.map(a => util.inspect(a, {colors: true})).join(" ");
      consoleLogBuf += line + "\n";
    };

    const res = await evaluate(cmd);

    const response = consoleLogBuf + util.inspect(res, {colors: true});

    globalThis.consoleLogBuf = "";

    return new Response(response);
  }
};
