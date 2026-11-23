# aws-chunked client fixtures

`../aws-chunked-sdk-fixtures.js` holds request bodies that S3 clients sent for streaming uploads,
which `aws-chunked-decoder-stream-test.js` decodes. Each client uploaded a payload to a local
server that recorded the request body as it arrived, after any HTTP-level chunked framing was
removed. The fixture keeps the aws-chunked framing verbatim and replaces each run of payload
bytes with its length; the payload is a sentence repeated to the decoded length.

| Client | Fixtures | `x-amz-content-sha256` |
|---|---|---|
| AWS SDK for JavaScript v3 (`@aws-sdk/client-s3` 3.1175.0) | `js-v3-*`: CRC32 (also over plain HTTP), SHA-256, CRC64NVME, an empty body | `STREAMING-UNSIGNED-PAYLOAD-TRAILER` |
| boto3 1.44.9 | `boto3-*`: CRC32, SHA-1 | `STREAMING-UNSIGNED-PAYLOAD-TRAILER` |
| AWS SDK for Go v2 (`service/s3` 1.116.0) | `go-v2-*`: CRC32, CRC32C, each body as one chunk | `STREAMING-UNSIGNED-PAYLOAD-TRAILER` |
| minio-go 7.4.1 | `minio-go-signed` | `STREAMING-AWS4-HMAC-SHA256-PAYLOAD` |
| | `minio-go-signed-trailer` | `STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER` |
| | `minio-go-unsigned-trailer` | `STREAMING-UNSIGNED-PAYLOAD-TRAILER` |

`@aws-sdk/client-s3` 3.1175.0 identifies itself as `aws-sdk-js/3.1174.0` in its user agent, so
that is the version the `js-v3-*` fixtures record.

The AWS SDKs send aws-chunked uploads only with an unsigned payload and a trailing checksum, for a
body they cannot hash ahead of time. minio-go signs each chunk when it uploads over plain HTTP. Its
checksum trailers end in LF before the CRLF.

The signed fixtures are signed with AWS's documented example credentials, `AKIDEXAMPLE` and
`wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY`, in `us-east-1`. The decoder does not verify
signatures, so the test checks only that it decodes these bodies.

To regenerate, run `./generate.sh`. It needs `node`, `python3`, `uv`, `go` and `openssl`, and
fetches the clients at the versions above. The signatures change on every run, since they cover
the request time.

- `capture.py` is the recording server.
- `js-v3-client.mjs`, `boto3-client.py` and `go-client.go` upload the payloads.
- `to-js.py` writes the fixture module, checking that each fixture rebuilds its recorded body.
