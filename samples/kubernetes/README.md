# A workerd cluster on Kubernetes

This sample runs a wrangler project on three workerd pods in workerd's experimental cluster mode.
The pods share an S3-compatible bucket. The project is copied into a container image built from a
base image that contains only workerd. Kubernetes rolls out new image versions.

Each Durable Object runs on one pod at a time. Any pod can accept a request for any object and send
it to the pod that runs the object. When Kubernetes stops a pod for a rollout, scale-down, or node
drain, the pod hands its objects to the other pods before it exits. Requests continue to work.

## Files

- `base/Dockerfile`: builds the base image. It contains workerd on a distroless image, runs as a
  non-root user, and serves the wrangler project in `/app` on port 8080.
- `app/`: contains the app as a wrangler project. `GET /` returns `hello world`.
  `GET /counter/<name>` adds one to the Durable Object counter named `<name>`. The response reports
  which pod accepted the request and which pod runs the object. `app/Dockerfile` builds the app
  image from the base image and the project.
- `bucket/`: contains a wrangler project that serves a fake S3 implementation. It is a stand-in
  bucket for this sample. Do not use it in production.
- `kubernetes.yaml`: defines the workerd Deployment, its Service, a PodDisruptionBudget,
  NetworkPolicies for the cluster port and demo bucket, and the demo bucket.
- `client.yaml`: defines a pod to send requests from.

**The stand-in bucket is a single pod with no durable storage.** Its data is in an `emptyDir`
volume. If its pod is deleted, evicted, or drained, the cluster loses all its state: every count
starts over, requests fail, and every workerd pod exits and restarts when its lease record
disappears. Never drain the stand-in bucket's node in a test you care about.

## Try it

You need Docker with BuildKit (Docker 23 or later, or `docker buildx build`), `kubectl`, OpenSSL,
and a Kubernetes cluster such as [kind](https://kind.sigs.k8s.io/).

The base image needs a workerd version that can join a cluster with `--cluster-addr` alone. By
default, its build installs the latest workerd from npm. Until a release includes this feature,
that build fails with "This workerd runs, but is too old to join a cluster with --cluster-addr", so
build workerd from source. Build it on Linux for the architecture of the cluster's nodes, with a C
library no newer than Debian 13's. From the repository root:

```sh
bazel build //src/workerd/server:workerd
mkdir -p /tmp/workerd-binary
install -m 755 bazel-bin/src/workerd/server/workerd /tmp/workerd-binary/workerd
strip /tmp/workerd-binary/workerd
```

Stripping shrinks the binary from about 380 MB to about 180 MB. Then, from the repository root,
build the three images:

```sh
docker build -t workerd-base \
  --build-arg WORKERD_SOURCE=local --build-context workerd-binary=/tmp/workerd-binary \
  samples/kubernetes/base
docker build -t workerd-sample:v1 samples/kubernetes/app
docker build -t workerd-sample-bucket:v1 samples/kubernetes/bucket
```

The manifest uses these tags for the app and bucket images. Give each new app or bucket build a new
tag, such as `v2`, and update the tag in `kubernetes.yaml`. With `imagePullPolicy: IfNotPresent`, a
node keeps using an image that it already has under the same tag.

Make the app and bucket images available to the cluster. For example, run
`kind load docker-image workerd-sample:v1 workerd-sample-bucket:v1`. You can instead push the
images to a registry and update `image:` in `kubernetes.yaml`. Then create the cluster key and
apply the manifest:

```sh
kubectl create secret generic workerd-cluster --from-literal=key="$(openssl rand -hex 32)"
kubectl apply -f samples/kubernetes/kubernetes.yaml
kubectl rollout status deployment/workerd
```

A workerd pod exits if it cannot read the bucket at startup, so if the demo bucket is not ready
yet, a workerd pod can restart once. The pods pass the bucket credentials as plain values. The demo
bucket accepts any credentials.

Send requests from inside the cluster. The client pod meets the "restricted" Pod Security
Standard, so it is admitted in namespaces that enforce it, too:

```sh
kubectl apply -f samples/kubernetes/client.yaml
kubectl wait --for=condition=Ready --timeout=2m pod/client
kubectl exec client -- curl -s http://workerd/
kubectl exec client -- sh -c 'for i in 1 2 3 4 5; do curl -s http://workerd/counter/alice; echo; done'
```

Each workerd process chooses a random ID on its first request. The count increases by one for each
request. `objectPod` stays the same regardless of which pod accepts the request.

Now send requests for two minutes while Kubernetes replaces every pod. The `awk` program counts
the requests, the ones that did not return 200, and the counts that did not go up by exactly one:

```sh
kubectl exec client -- sh -c 'end=$(( $(date +%s) + 120 ))
  while [ "$(date +%s)" -lt "$end" ]; do
    curl -s -w " %{http_code}\n" http://workerd/counter/alice
  done' | awk '{ n++ }
  $NF != 200 { failed++; next }
  match($0, /"count":[0-9]+/) {
    count = substr($0, RSTART + 8, RLENGTH - 8) + 0
    if (last && count != last + 1) wrong++
    last = count
  }
  END { printf "%d requests, %d failed, %d counts skipped or repeated\n", n, failed, wrong }' &
kubectl rollout restart deployment/workerd
kubectl rollout status deployment/workerd
wait
```

It prints, for example, `2200 requests, 0 failed, 0 counts skipped or repeated`. The number of
requests depends on the client and the network, so expect about that many. `objectPod` moves to
the new pods.

To remove everything the sample created:

```sh
kubectl delete -f samples/kubernetes/client.yaml -f samples/kubernetes/kubernetes.yaml
kubectl delete secret workerd-cluster
```

## Building an image for your own project

Replace the contents of `app/` with your wrangler project. Keep `app/Dockerfile` and
`app/.dockerignore`. Then build the app image as shown above. The base image serves
`/app/wrangler.json`, so:

- Set `"no_bundle": true`. The image has no esbuild. `main` must be a `.js` or `.mjs` ES module.
  The modules it imports must be files in its directory or below. Every `.js`, `.mjs`, `.cjs`,
  `.wasm`, `.txt`, `.html`, `.sql`, and `.bin` file there becomes a module.
- Use only the wrangler fields that workerd supports in a cluster: `$schema`, `name`, `main`,
  `compatibility_date`, `compatibility_flags`, `vars`, `durable_objects`, `exports`, `migrations`,
  `triggers` (`crons`), and `no_bundle`. Any other field is an error, including `kv_namespaces`
  and `queues`.
- With `triggers.crons`, also add `--directory-path=<name>-cron-storage=/data` to
  `kubernetes.yaml`, where `<name>` is the project's `name`. The cron schedule's state is kept
  there, beside the Durable Objects.
- To use `wrangler.jsonc`, copy it to `/app/wrangler.json`. Add
  `COPY wrangler.jsonc /app/wrangler.json` to the Dockerfile. Comments and trailing commas are
  allowed in either file.
- Keep `.wrangler` out of the image, as `app/.dockerignore` does.

To roll a new version of the same project into a running cluster, follow
[Rolling out a new version](#rolling-out-a-new-version). Start a different project on a fresh
cluster, or on the same one with a fresh bucket prefix in `WORKERD_CLUSTER_BUCKET`. Rolling a
different project into a running cluster fails some requests with 500 errors, such as
`Worker "counter" not found`, while old pods forward requests to new pods that no longer serve the
old project.

## How it fits together

- **Joining.** `--cluster-addr=$(POD_IP):7000` makes each pod a cluster member. Pods listen for
  each other on port 7000 of their pod IPs. This is also the address they advertise. The other
  cluster settings use their defaults.
- **Key and bucket.** Each pod reads the cluster key from the file named by
  `WORKERD_CLUSTER_KEY_FILE`. The Secret is mounted as this file. Each pod reads the bucket from
  `WORKERD_CLUSTER_BUCKET`. The bucket endpoint and credentials come from `AWS_ENDPOINT_URL`,
  `AWS_ACCESS_KEY_ID`, and `AWS_SECRET_ACCESS_KEY`. The region is `us-east-1` unless `AWS_REGION`
  sets another region. The image contains no secrets, so the same image can join any cluster.
- **Local storage.** In a cluster, a wrangler project's Durable Objects are stored on the disk
  service `do-storage`. A project without Durable Objects also defines this service.
  `--directory-path=do-storage=/data` puts it on an `emptyDir` volume. Each pod stores its copy of
  the objects it runs there. The bucket holds the authoritative copy, so the pods do not need
  persistent volumes. The images have no shell. To look at a pod's data, attach a debug
  container with `kubectl debug` and mount the volume read-only.
- **Readiness.** `GET /ready` on port 7000 returns 200 while the pod serves. It returns 503 while
  the pod starts or stops, so the Service stops sending requests to a stopping pod.
- **Stopping.** On SIGTERM, a pod hands its objects to the other pods within 8 seconds and then
  exits. Before that, a `preStop` hook makes the kubelet wait 20 seconds while the pod keeps
  serving. Once a pod starts terminating, a cloud load balancer in front of the Service can keep
  sending it requests for up to about 20 seconds, which would fail once workerd has exited. The
  hook uses the kubelet's `sleep` action, since the image has no shell, and needs Kubernetes 1.30
  or later. With only a ClusterIP Service, as in this sample, Kubernetes stops routing to the
  pod almost as soon as it starts terminating, so the hook is not needed and you can remove it.
  `terminationGracePeriodSeconds` (40) must cover the sleep and the 8 seconds after SIGTERM.
- **Rollouts.** A rollout starts one new pod at a time (`maxSurge: 1`, `maxUnavailable: 0`). It
  stops an old pod only after the new pod has been ready for 10 seconds (`minReadySeconds: 10`).
  This is longer than a pod needs to hand off its objects, so only one pod hands off at a time.
  The spacing counts from when each pod gets SIGTERM, 20 seconds after it starts terminating, so
  two or three old pods are often terminating at once while their handoffs still start more than
  10 seconds apart.
- **Drains.** The PodDisruptionBudget allows a node drain or another eviction to stop a workerd
  pod only while all workerd pods are ready. It does not wait for `minReadySeconds`. Once a stopped
  pod's replacement is ready, the next pod can be evicted while the first pod is still handing off
  its objects. The scheduler prefers to spread the pods across nodes, so one node holds few of
  them. Scaling down by more than one pod stops several pods at once. The stand-in bucket has no
  PodDisruptionBudget and no durable storage: draining its node loses the cluster's state and
  restarts every workerd pod.
- **`fetch()` and JS RPC.** A `fetch()` call to an object that moves is sent to its new pod. A JS
  RPC call that reaches a stopping pod through a stub can fail once with a retryable error. The
  stub's next call finds the object. `app/src/index.js` uses `fetch()`.
- **Network.** Anyone who can reach port 7000 with the cluster key can call every service in the
  pod. The traffic is not encrypted. The NetworkPolicies allow only the workerd pods to connect to
  port 7000 and the demo bucket. They take effect only if the cluster's network plugin enforces
  NetworkPolicy. They never block connections from a pod's own node.

## Using a real bucket

Remove the `fake-s3` Deployment, Service, and NetworkPolicy from `kubernetes.yaml`. Set
`WORKERD_CLUSTER_BUCKET` to `s3://<bucket>/<prefix>`. Set `AWS_ENDPOINT_URL` and `AWS_REGION` for
your provider, such as R2 or S3. Take `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY` from a Secret.
You can instead mount a credentials file and set `AWS_CREDENTIALS_FILE`. All pods in a cluster must
be in one region. For Cloud Storage, set `WORKERD_CLUSTER_BUCKET` to `gs://<bucket>/<prefix>` and
drop the `AWS_*` variables: the pods then take tokens from GKE Workload Identity, or from the
service account key that `GOOGLE_APPLICATION_CREDENTIALS` names.

Protect write access to the bucket as you protect the cluster key. Anyone who can write to the
bucket can publish code that every pod runs. Give write credentials only to the pods and whoever
deploys.

## Rolling out a new version

During a rollout, old and new pods run side by side. A request for an object goes to the pod that
runs it, whether that pod is old or new. Each version must therefore work with the previous
version:

- Keep the project's `name` and its Durable Object classes' storage keys. A class's objects are
  stored under `<name>-<key>`. The key is the name used when the class was created. It stays the
  same when the class is renamed. A new name or key starts with empty objects.
- Keep RPC compatible between versions. A call from an object on a new pod can reach an object on
  an old pod, and the reverse is also true. Add a method or field in one version, then start using
  it in a later version. Remove it only after no pod runs a version that uses it.
- Objects move from old pods to new pods, so new code reads data that old code wrote. Old code can
  also read data that new code wrote. An object can move from a new pod back to an old pod during
  the rollout, for example when a new pod fails. A rollback moves every object back. Keep stored
  data readable by the previous version.

## Limitations

- Cluster mode is experimental and needs `--experimental`.
- A cluster joined with `--cluster-addr` uses the default for every setting except the address,
  key, and bucket. For example, a pod takes up to 8 seconds to stop.
- Durable Objects with containers need a Docker engine, which a pod does not normally have.
- Pod IPs must be IPv4. `--cluster-addr=$(POD_IP):7000` is not a valid IPv6 address.
