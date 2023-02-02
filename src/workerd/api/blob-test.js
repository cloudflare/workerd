function assertEqual(a, b) {
  if (a !== b) {
    throw new Error(a + " !== " + b);
  }
}

export default {
  async test(ctrl, env, ctx) {
    let blob = new Blob(["foo", new TextEncoder().encode("bar"), "baz"]);
    assertEqual(await blob.text(), "foobarbaz");
    assertEqual(new TextDecoder().decode(await blob.arrayBuffer()), "foobarbaz");
    assertEqual(blob.type, "");

    let blob2 = new Blob(["xx", blob, "yy", blob], {type: "application/whatever"});
    assertEqual(await blob2.text(), "xxfoobarbazyyfoobarbaz");
    assertEqual(blob2.type, "application/whatever");

    let blob3 = new Blob();
    assertEqual(await blob3.text(), "");

    let slice = blob2.slice(5, 16);
    assertEqual(await slice.text(), "barbazyyfoo");
    assertEqual(slice.type, "");

    let slice2 = slice.slice(-5, 1234, "type/type");
    assertEqual(await slice2.text(), "yyfoo");
    assertEqual(slice2.type, "type/type");

    assertEqual(await blob2.slice(5).text(), "barbazyyfoobarbaz");
    assertEqual(await blob2.slice().text(), "xxfoobarbazyyfoobarbaz");
    assertEqual(await blob2.slice(3, 1).text(), "");

    {
      let stream = blob.stream();
      let reader = stream.getReader();
      let readResult = await reader.read();
      assertEqual(readResult.done, false);
      assertEqual(new TextDecoder().decode(readResult.value), "foobarbaz");
      readResult = await reader.read();
      assertEqual(readResult.value, undefined);
      assertEqual(readResult.done, true);
      reader.releaseLock();
    }

    let before = Date.now();

    let file = new File([blob, "qux"], "filename.txt");
    assertEqual(file instanceof Blob, true);
    assertEqual(await file.text(), "foobarbazqux");
    assertEqual(file.name, "filename.txt");
    assertEqual(file.type, "");
    if (file.lastModified < before || file.lastModified > Date.now()) {
      throw new Error("incorrect lastModified");
    }

    let file2 = new File(["corge", file], "file2", {type: "text/foo", lastModified: 123});
    assertEqual(await file2.text(), "corgefoobarbazqux");
    assertEqual(file2.name, "file2");
    assertEqual(file2.type, "text/foo");
    assertEqual(file2.lastModified, 123);

    try {
      new Blob(["foo"], {endings: "native"});
      throw new Error("use of 'endings' should throw");
    } catch (err) {
      if (!err.message.includes("The 'endings' field on 'Options' is not implemented.")) {
        throw err;
      }
    }

    // Test type normalization.
    assertEqual(new Blob([], {type: "FoO/bAr"}).type, "foo/bar");
    assertEqual(new Blob([], {type: "FoO\u0019/bAr"}).type, "");
    assertEqual(new Blob([], {type: "FoO\u0020/bAr"}).type, "foo /bar");
    assertEqual(new Blob([], {type: "FoO\u007e/bAr"}).type, "foo\u007e/bar");
    assertEqual(new Blob([], {type: "FoO\u0080/bAr"}).type, "");
    assertEqual(new File([], "foo.txt", {type: "FoO/bAr"}).type, "foo/bar");
    assertEqual(blob2.slice(1, 2, "FoO/bAr").type, "foo/bar");
  }
}
