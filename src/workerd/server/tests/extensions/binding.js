export function wrap(env) {
  if (!env.secret) {
    throw new Error("secret internal binding is not specified");
  }

  return {
    tryOpen(key) {
      return key === env.secret;
    }
  }
}


export default wrap;
