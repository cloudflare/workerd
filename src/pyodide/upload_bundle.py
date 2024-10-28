import sys
from os import environ

from boto3 import client


def main(path, key):
    s3 = client(
        "s3",
        endpoint_url=f"https://{environ['R2_ACCOUNT_ID']}.r2.cloudflarestorage.com",
        aws_access_key_id=environ["R2_ACCESS_KEY_ID"],
        aws_secret_access_key=environ["R2_SECRET_ACCESS_KEY"],
        region_name="auto",
    )

    s3.upload_file(str(path), "pyodide-capnp-bin", key)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
