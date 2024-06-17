//@ts-ignore
import { default as Stdin } from "workerd:stdin";
//@ts-ignore
import { default as UnsafeEval } from "internal:unsafe-eval";

// https://vane.life/2016/04/03/eval-locally-with-persistent-context/
var __EVAL = (s: string) => UnsafeEval.eval(`void (__EVAL = ${__EVAL.toString()}); ${s}`);

function evaluate(expr: string) {
    try {
        const result = __EVAL(expr);
        console.log(result);
    } catch(err: any) {
        console.log(expr, 'ERROR:', err.message);
    }
}

export default function repl() {
  while(true) {
    const query: string = Stdin.getline();
    evaluate(query);
  }
}
