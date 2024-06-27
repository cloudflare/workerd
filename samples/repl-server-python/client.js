const repl = require("repl");

const myeval = async (cmd, ctx, fname, callback) => {
  const res = await fetch('http://localhost:8080', {
    method: 'POST',
    headers: {
      'Accept': 'application/json',
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({cmd})
  });

  callback(null, await res.text());
}

repl.start({
  eval: myeval,
  writer: a => a
});
