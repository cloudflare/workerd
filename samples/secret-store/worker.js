
const dec = new TextDecoder();
async function getSecret(secret) {
  return dec.decode(await crypto.subtle.exportKey("raw", secret));
}

export default {
  async fetch(req, env) {
    const SECRET = await getSecret(env.SECRET);

    console.log(SECRET);

    return new Response("ok");
  }
};
