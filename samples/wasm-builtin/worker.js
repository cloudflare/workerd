import {add} from "node:add";

export default {
    async fetch(request) {
        const a = 3;
        const b = 7;
        const res = add(a, b);
        return new Response(`${a} + ${b} = ${res}`);
    }
};
