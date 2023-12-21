export default {
  fetch(req) {
    // The req.url is currently used to communicate the identity of the secret to get.
    if (req.url === "foo") {
      // TODO(soon): Currently this returns the secret data directly as the
      // body of the payload. Later we'll want this to be more structured,
      // possibly returning a JSON document with additional metadata such
      // as cache TTL, secret type, etc.
      return new Response("THIS IS A SECRET... SHHHH!");
    } else {
      // For a secret that doesn't exist, return an error.
      throw new Error("Nothing");
    }
  }
};
