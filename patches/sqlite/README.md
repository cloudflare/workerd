# Patching SQLite

The patches for SQLite are applied against the plain source, not the amalgamated source.

SQLite comes in two forms. First, there's the "plain" form. It looks a
lot like a typical open-source C project: there's a bunch of .c and .h
files, a Makefile, a configure script, and various other files.
workerd consumes this form.

Second, there's the "amalgamation". This is all of SQLite combined
into two .c and two .h files. https://www.sqlite.org/download.html
puts the amalgamation front and center, but don't be fooled.

To update to a new SQLite version, obtain the new SQLite in plain
form. Apply the patches to SQLite, fixing as necessary, and replace
any patches that needed modification with their fixed versions.

Example, assuming the new SQLite has been downloaded into the current
directory:

```bash
export VERSION=3470000
unzip sqlite-src-$VERSION.zip
mv sqlite-src-$VERSION sqlite-src-pristine
unzip sqlite-src-$VERSION.zip  # yes, again
mv sqlite-src-$VERSION sqlite-src-modified
```

Now patch:

```bash
cd sqlite-src-modified
patch -p1 < /path/to/workerd/patches/sqlite/0001-row-counts-plain.patch
./configure && make test
```

Make sure the tests pass. If the patch needed any modification, regenerate it:

```bash
diff -u5 -r sqlite-src-pristine sqlite-src-modified \
    | grep -v "Only in sqlite-src-modified" \
    > /path/to/workerd/patches/sqlite/0001-row-counts-plain.patch
```

Repeat for each patch.

Finally, switch the build to the new version. In
`build/deps/shared_deps.jsonc`, set `freeze_version` and `freeze_year`
of `sqlite3` to the new release, remove its `freeze_sha256`, and
update its `patches` if any were added or removed. Then regenerate the
Bazel definition:

```bash
just update-deps sqlite3
```

This downloads the new release and, if it is the newest one, checks it
against the hash on SQLite's download page. Copy the `sha256` that it
writes to `build/deps/gen/shared_deps.MODULE.bazel` into
`freeze_sha256`.
