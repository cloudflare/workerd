import { default as foo } from 'foo';
const {
  Buffer,
  util,
} = foo;

export default {
  async fetch(request) {
    console.log(Buffer);
    console.log(util);
    return new Response("ok");
  }
};
