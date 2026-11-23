// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Uploads the fixture payloads with the AWS SDK for Go v2 and minio-go.
package main

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"io"
	"net/http"
	"os"
	"strings"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/credentials"
	"github.com/aws/aws-sdk-go-v2/service/s3"
	"github.com/aws/aws-sdk-go-v2/service/s3/types"
	"github.com/minio/minio-go/v7"
	miniocreds "github.com/minio/minio-go/v7/pkg/credentials"
)

const sentence = "The quick brown fox jumps over the lazy dog. "
const accessKey, secretKey = "AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"

func payload(n int) []byte {
	return []byte(strings.Repeat(sentence, n/len(sentence)+2)[:n])
}

// onlyReader hides Seek, so the SDK can't rewind or hash the body ahead of time.
type onlyReader struct{ io.Reader }

func must(err error) {
	if err != nil {
		panic(err)
	}
}

func main() {
	httpPort, httpsPort, certPath := os.Args[1], os.Args[2], os.Args[3]
	ctx := context.Background()
	// The recording server's certificate is the only one trusted.
	cert, err := os.ReadFile(certPath)
	must(err)
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(cert) {
		panic("no certificate in " + certPath)
	}
	trusting := &http.Client{Transport: &http.Transport{TLSClientConfig: &tls.Config{RootCAs: roots}}}

	client := s3.New(s3.Options{
		Region:       "us-east-1",
		BaseEndpoint: aws.String("https://127.0.0.1:" + httpsPort),
		UsePathStyle: true,
		Credentials:  credentials.NewStaticCredentialsProvider(accessKey, secretKey, ""),
		HTTPClient:   trusting,
	})
	put := func(name string, size int, algorithm types.ChecksumAlgorithm) {
		_, err := client.PutObject(ctx, &s3.PutObjectInput{
			Bucket: aws.String("fixtures"), Key: aws.String(name),
			Body: onlyReader{bytes.NewReader(payload(size))}, ContentLength: aws.Int64(int64(size)),
			ChecksumAlgorithm: algorithm,
		})
		must(err)
	}
	put("go-v2-crc32", 70000, types.ChecksumAlgorithmCrc32)
	put("go-v2-crc32c", 1000, types.ChecksumAlgorithmCrc32c)

	minioPut := func(name string, secure bool, size int, options minio.PutObjectOptions) {
		endpoint := "127.0.0.1:" + httpPort
		transport := http.DefaultTransport
		if secure {
			endpoint = "127.0.0.1:" + httpsPort
			transport = trusting.Transport
		}
		mc, err := minio.New(endpoint, &minio.Options{
			Creds: miniocreds.NewStaticV4(accessKey, secretKey, ""), Secure: secure,
			Region: "us-east-1", Transport: transport, BucketLookup: minio.BucketLookupPath,
			TrailingHeaders: options.Checksum.IsSet(),
		})
		must(err)
		_, err = mc.PutObject(ctx, "fixtures", name, onlyReader{bytes.NewReader(payload(size))}, int64(size), options)
		must(err)
	}
	minioPut("minio-go-signed", false, 70000, minio.PutObjectOptions{})
	minioPut("minio-go-signed-trailer", false, 70000, minio.PutObjectOptions{AutoChecksum: minio.ChecksumCRC32C, Checksum: minio.ChecksumCRC32C})
	minioPut("minio-go-unsigned-trailer", true, 70000, minio.PutObjectOptions{AutoChecksum: minio.ChecksumCRC32C, Checksum: minio.ChecksumCRC32C})
}
