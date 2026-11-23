# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Uploads the fixture payloads with boto3."""

import io
import sys

import boto3
from botocore.config import Config

SENTENCE = b"The quick brown fox jumps over the lazy dog. "


def payload(n):
    return (SENTENCE * (n // len(SENTENCE) + 2))[:n]


class Unseekable(io.RawIOBase):
    """A body boto3 can't rewind or measure, like a request body being forwarded."""

    def __init__(self, data):
        self.data = io.BytesIO(data)

    def readable(self):
        return True

    def read(self, n=-1):
        return self.data.read(n)


# The recording server's certificate is the only one trusted.
http_port, https_port, cert_path = sys.argv[1:4]
s3 = boto3.client(
    "s3",
    endpoint_url=f"https://127.0.0.1:{https_port}",
    region_name="us-east-1",
    aws_access_key_id="AKIDEXAMPLE",
    aws_secret_access_key="wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY",
    verify=cert_path,
    config=Config(s3={"addressing_style": "path"}),
)
s3.put_object(
    Bucket="fixtures",
    Key="boto3-crc32",
    Body=Unseekable(payload(70000)),
    ContentLength=70000,
)
s3.put_object(
    Bucket="fixtures",
    Key="boto3-sha1",
    Body=Unseekable(payload(1000)),
    ContentLength=1000,
    ChecksumAlgorithm="SHA1",
)
