# FormData × streams

Multipart parsing FROM streamed bodies and FormData serialized INTO a
stream body — under both stream implementations. **The tests are the
normative artifact.** The general FormData surface (W3C API matrix,
urlencoded, entry semantics) is owned by `src/workerd/api/tests/
form-data-test.js`; this suite owns the STREAMS interaction only.

Both cells set `formdata_parser_supports_files` so multipart file
entries parse as File objects.

## Coverage (parity — no divergences observed)

| Test | Shape |
| --- | --- |
| `parseMultipartSingleChunk` | whole body in one stream chunk |
| `parseMultipartAwkwardChunkSplits` | chunk boundaries inside the boundary marker, inside a header, inside a value, inside the closing marker |
| `parseMultipartBytewiseChunks` | every byte its own chunk (worst-case reassembly) |
| `parseFilesFromStreamedMultipart` | File entries out of a streamed body; content read back via file.text() |
| `parseMultipartFromIdentityStream` | identity body fed by a concurrent writer |
| `parseLargeStreamedMultipart` | 100 fields + a 256 KiB file, 8 KiB chunks |
| `erroringBodyRejectsFormData` | source error rejects formData() |
| `serializedFormDataBodyRoundTrips` | Response(FormData) body drained as a stream and reparsed (boundary from the generated content-type) |
