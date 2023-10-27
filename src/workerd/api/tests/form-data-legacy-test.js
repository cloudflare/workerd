export const sendFilesInFormdata = {
  async test() {
    const INPUT = `--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: form-data; name="foo"; filename="foo.txt"
Content-Type: application/octet-stream

foo-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: form-data; name="bar"; filename="bar-renamed.txt"
Content-Type: application/octet-stream

bar1-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: form-data; name="bar"; filename="bar2.txt"
Content-Type: text/bary

bar2-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: form-data; name="baz"; filename="baz"
Content-Type: text/bazzy

baz-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: form-data; name="qux"; filename="qux%0A%22\\.txt"
Content-Type: application/octet-stream

qux-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a--
`;

    const req = new Request('https://example.org', {
      method: 'POST',
      body: INPUT,
      headers: {
        'content-type': 'multipart/form-data;boundary=2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a'
      }
    });

    const form = await req.formData();

    form.set("foo", new File(["foo-content"], "foo.txt"));
    form.append("bar", new File(["bar1-content"], "bar1.txt"), "bar-renamed.txt");
    form.append("bar", new File(["bar2-content"], "bar2.txt", {type: "text/bary"}));
    form.append("baz", new Blob(["baz-content"], {type: "text/bazzy"}));
    form.set("qux", new Blob(["qux-content"]), "qux\n\"\\.txt");

    {
      let resp = new Response(form);
      let text = await resp.text();
      let roundtrip = await new Response(text, {headers: resp.headers}).formData();
      if (roundtrip.get("foo") != "foo-content") {
        throw new Error("expected round-trip turns into string (wrong, but backwards-compatible)");
      }
    }

  }
};

