import argparse
import requests
import time
import sys

def parse_args():
    parser = argparse.ArgumentParser()

    parser.add_argument("pr_id", help="Pull Request ID")
    parser.add_argument("sha", help="Commit SHA")
    parser.add_argument("run_attempt", help="# of Run Attempt")
    parser.add_argument("branch_name", help="PR's Branch Name")
    parser.add_argument("URL", help="URL to submit build task")
    parser.add_argument("client_id", help="CF Access client id")
    parser.add_argument("secret", help="CF Access client secret")

    return parser.parse_args()

if __name__ == "__main__":
    args = parse_args()

    # Submit build job
    headers = {
        "CF-Access-Client-Id": args.client_id,
        "CF-Access-Client-Secret": args.secret
    }

    payload = {
        "pr_id": args.pr_id,
        "commit_sha": args.sha,
        "run_attempt": args.run_attempt,
        "branch_name": args.branch_name
    }

    workflow_id = ''
    try:
        resp = requests.post(args.URL, headers=headers, json=payload)
        resp.raise_for_status()

        workflow_id = resp.json().id
    except Exception as err:
        print(f"Unexpected error {err=}, {type(err)=}")
        sys.exit(1)

    time.sleep(30)

    # Poll build status
    failed_requests = 0
    FAILED_REQUEST_LIMIT = 10

    while failed_requests < FAILED_REQUEST_LIMIT:
        try:
            resp = requests.get(args.URL, headers=headers, params={ 'id': workflow_id })
            resp.raise_for_status()

            status = resp.json().status
            if status == "errored":
                sys.exit(1)
            elif status == "complete":
                break

        except Exception as err:
            print(f"Unexpected error {err=}, {type(err)=}")
            failed_requests = failed_requests + 1
        time.sleep(30)

    if failed_requests == FAILED_REQUEST_LIMIT:
        sys.exit(1)

    print("Internal build succeeded")

