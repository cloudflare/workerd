# Run a workerd cluster on Kubernetes

This sample runs a Wrangler project on three workerd pods in the experimental cluster mode of
workerd. The app image is the [workerd container image](../../docker/README.md),
`docker.io/cloudflare/workerd`, with the project copied into it. Kubernetes rolls out new versions
of the app image.

The sample works on many Kubernetes clusters, but it requires a load balancer implementation that
can provision Services of type `LoadBalancer`. The app is served at the load balancer's address.

The pods store the cluster's state in an S3-compatible bucket. The sample doesn't use a hosted
object storage service: a single [RustFS](https://rustfs.com/) instance in the cluster emulates an
S3 bucket. For production use, use a real bucket, such as R2, S3, or Cloud Storage. For more
information, see [Use a real bucket](#use-a-real-bucket).

Each Durable Object runs on one pod at a time. Any pod can accept a request for any object, and it
sends the request to the pod that runs the object. When Kubernetes stops a pod for a rollout, a
scale-down, or a node drain, the pod hands off its objects to the other pods before it exits, so
requests keep working.

## What's in the sample

The sample contains the following files:

- `app/`: the app as a Wrangler project. `GET /` returns `hello world`.
  <code>GET /counter/<var>NAME</var></code> increments the Durable Object counter named
  <var>NAME</var>. The response shows which pod accepted the request and which pod runs the object.
  `app/Dockerfile` builds the app image.
- `kubernetes.yaml`: the manifest. It defines the workerd Deployment, a LoadBalancer Service for
  it, a PodDisruptionBudget, and a NetworkPolicy for the cluster port. It also defines the demo
  bucket: a RustFS StatefulSet with one pod and a 1 GiB persistent volume, a Service for it, a Job
  that creates the bucket, and a NetworkPolicy.

> **Caution:** The demo bucket has no redundancy. If you lose its volume, you lose the cluster's
> state. The bucket's objects survive a restart of the RustFS pod, but while the pod is down,
> requests for Durable Objects fail, and the workerd pods exit and restart until RustFS responds
> again. After that, the workerd pods recover without intervention. A restart that takes a few
> seconds causes few or no failed requests. An outage of a minute fails every request until RustFS
> is back.

## Before you begin

To run the sample, you need the following:

- Docker 23 or later, or an earlier Docker with BuildKit through `docker buildx build`
- `kubectl`
- OpenSSL
- curl
- A Kubernetes cluster that has a default StorageClass and a load balancer implementation

Managed Kubernetes services and k3s provide a default StorageClass and a load balancer. A
[kind](https://kind.sigs.k8s.io/) cluster needs
[cloud-provider-kind](https://github.com/kubernetes-sigs/cloud-provider-kind) for the load balancer.

## Run the sample

To run the sample, do the following from the repository root:

1. Build the app image:

   ```sh
   docker build -t workerd-sample:v1 samples/kubernetes/app
   ```

   The app image builds on `docker.io/cloudflare/workerd:latest`. To use a workerd that you build
   from source instead, see [Use a workerd built from source](#use-a-workerd-built-from-source).

1. Make the app image available to the cluster. For example, for a kind cluster, run the following
   command:

   ```sh
   kind load docker-image workerd-sample:v1
   ```

   Alternatively, push the image to a registry and set `image:` in `kubernetes.yaml` to the image's
   name in the registry. The cluster pulls the RustFS and curl images from Docker Hub.

1. Create Secrets for the cluster key and for the bucket credentials, which RustFS and the workerd
   pods share:

   ```sh
   kubectl create secret generic workerd-cluster --from-literal=key="$(openssl rand -hex 32)"
   kubectl create secret generic bucket-credentials \
     --from-literal=access-key="$(openssl rand -hex 16)" \
     --from-literal=secret-key="$(openssl rand -hex 32)"
   ```

1. Apply the manifest, and wait for the bucket and the workerd pods.

   > **Caution:** On a cloud provider, the load balancer's address is public. Anyone can reach the
   > app until you [clean up](#clean-up).

   ```sh
   kubectl apply -f samples/kubernetes/kubernetes.yaml
   kubectl wait --for=condition=complete --timeout=5m job/rustfs-bucket
   kubectl rollout status deployment/workerd
   ```

   After RustFS responds, the Job creates the bucket `cluster`. A workerd pod exits if it can't read
   the bucket when it starts, so a pod that starts before the bucket exists restarts until the
   bucket is there.

1. Wait for the load balancer's address, and save it in the `lb` shell variable:

   ```sh
   kubectl wait --for=jsonpath='{.status.loadBalancer.ingress}' --timeout=5m service/workerd
   lb=$(kubectl get service workerd \
     -o jsonpath='{.status.loadBalancer.ingress[0].ip}{.status.loadBalancer.ingress[0].hostname}')
   ```

   If the wait times out, the cluster probably has no load balancer.

1. Send requests to the app:

   ```sh
   curl -s http://$lb/
   for i in 1 2 3 4 5; do curl -s http://$lb/counter/alice; echo; done
   ```

   The output is similar to the following:

   ```none
   hello world
   {"name":"alice","count":1,"requestPod":"93bd99e4","objectPod":"50d1240f"}
   {"name":"alice","count":2,"requestPod":"50d1240f","objectPod":"50d1240f"}
   {"name":"alice","count":3,"requestPod":"85459989","objectPod":"50d1240f"}
   {"name":"alice","count":4,"requestPod":"50d1240f","objectPod":"50d1240f"}
   {"name":"alice","count":5,"requestPod":"93bd99e4","objectPod":"50d1240f"}
   ```

   Each workerd process chooses a random ID when it handles its first request. `requestPod` is the
   ID of the pod that accepted the request, and `objectPod` is the ID of the pod that runs the
   object. The count increases by one with each request, and `objectPod` stays the same, whichever
   pod accepts the request.

1. Send requests for two minutes while Kubernetes replaces every pod:

   ```sh
   end=$(( $(date +%s) + 120 ))
   while [ "$(date +%s)" -lt "$end" ]; do
     curl -s -w " %{http_code}\n" http://$lb/counter/alice
   done | awk '{ n++ }
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

   The `awk` program counts the requests, the requests that didn't return 200, and the counts that
   didn't increase by exactly one. The output is similar to the following:

   ```none
   2200 requests, 0 failed, 0 counts skipped or repeated
   ```

   The number of requests depends on the network and on the computer that sends them. After the
   rollout, `objectPod` is the ID of one of the new pods.

### Use a workerd built from source

To use a workerd that you build from source, build it on Linux for the architecture of the
cluster's nodes, with a C library that's no newer than the one in Debian 13. Then build the workerd
image from it. For example, on an x86_64 machine, run the following commands from the repository
root:

```sh
bazel build //src/workerd/server:workerd
mkdir -p /tmp/workerd-image/amd64
install -m 755 bazel-bin/src/workerd/server/workerd /tmp/workerd-image/amd64/workerd
strip /tmp/workerd-image/amd64/workerd
docker buildx build --platform linux/amd64 --load -t workerd -f docker/Dockerfile /tmp/workerd-image
```

Stripping the binary shrinks it from about 390 MB to about 180 MB. To build the app image on this
workerd image, add `--build-arg WORKERD_IMAGE=workerd` to the `docker build` command in
[Run the sample](#run-the-sample).

## Clean up

To delete everything that the sample created, run the following commands:

```sh
kubectl delete -f samples/kubernetes/kubernetes.yaml
kubectl delete secret workerd-cluster bucket-credentials
kubectl delete pvc data-rustfs-0
```

Deleting the StatefulSet doesn't delete its volume, so the last command deletes the bucket's data.

## Build an image for your own project

To build an image for your own Wrangler project, do the following:

1. Replace the contents of `app/` with your project, but keep `app/Dockerfile` and
   `app/.dockerignore`.
1. Build the app image as in [Run the sample](#run-the-sample).

The workerd image serves the project in `/app` from its `wrangler.json` file, or from its
`wrangler.jsonc` file if it has no `wrangler.json` file. Your project must meet the following
requirements:

- Set `"no_bundle": true`. The image doesn't include esbuild.
- Make `main` a `.js` or `.mjs` ES module. The modules that it imports must be files in its
  directory or a subdirectory. Every `.js`, `.mjs`, `.cjs`, `.wasm`, `.txt`, `.html`, `.sql`, and
  `.bin` file in that directory tree becomes a module.
- Use only the Wrangler fields that workerd supports in a cluster: `$schema`, `name`, `main`,
  `compatibility_date`, `compatibility_flags`, `vars`, `durable_objects`, `exports`, `migrations`,
  `r2_buckets`, `d1_databases`, `workflows`, `triggers` (`crons`), `secrets_store_secrets`,
  `ratelimits`, `assets`, and `no_bundle`. Some of these fields require the `--experimental` flag,
  which the manifest passes. Any other field, including `kv_namespaces` and `queues`, is an error.
  The objects of an R2 bucket are stored in the cluster's bucket. Each pod counts `ratelimits`
  separately.
- Convert a `wrangler.toml` file to `wrangler.jsonc`. The image doesn't read `wrangler.toml`.
- Keep the `.wrangler` directory out of the image, as `app/.dockerignore` does.

To deploy a new version of the same project to a running cluster, see
[Roll out a new version](#roll-out-a-new-version). To run a different project, start a new cluster,
or use the same cluster with a new bucket prefix in `WORKERD_CLUSTER_BUCKET`. If you roll out a
different project to a running cluster, some requests fail with 500 errors, such as
`Worker "counter" not found`, while old pods forward requests to new pods that don't serve the old
project.

## How the sample works

The sample uses the following parts of workerd and Kubernetes:

- **Image.** The app image is the workerd image with the project in `/app`. The workerd image
  serves the project on port 8080, runs as a non-root user, and has no shell.
- **Joining.** The `--cluster-addr=$(POD_IP):7000` flag makes each pod a member of the cluster. The
  pods listen for each other on port 7000 of their pod IP addresses, and they advertise that
  address to each other. The other cluster settings use their defaults, except the stop timeout,
  which **Stopping** describes.
- **Key and bucket.** Each pod reads the cluster key from the file that `WORKERD_CLUSTER_KEY_FILE`
  names, where the manifest mounts the `workerd-cluster` Secret. Each pod reads the bucket's
  location from `WORKERD_CLUSTER_BUCKET`, and the bucket's endpoint and credentials from
  `AWS_ENDPOINT_URL`, `AWS_ACCESS_KEY_ID`, and `AWS_SECRET_ACCESS_KEY`. The region is `us-east-1`,
  unless `AWS_REGION` sets a different region. The image contains no secrets, so the same image can
  join any cluster.
- **Local storage.** The `--data-dir=/data` flag puts the project's state directory on an
  `emptyDir` volume. In a cluster, a Wrangler project's Durable Objects are stored in `/data/do`,
  on the disk service `do-storage`, which the cluster's deployments also use. The Cache API stores
  its entries under `/data` too. Each pod stores its copy of the objects that it runs there. The
  bucket holds the authoritative copy, so the pods don't need persistent volumes. The images have no
  shell, so to inspect a pod's data, attach a debug container with `kubectl debug` and mount the
  volume read-only.
- **Readiness.** On port 7000, `GET /ready` returns 200 while the pod serves requests, and 503
  while the pod starts or stops. The readiness probe uses it, so the Service stops sending requests
  to a pod that's stopping.
- **Stopping.** When a pod receives SIGTERM, it keeps serving for 20 seconds, as
  `WORKERD_SHUTDOWN_DELAY_MS` sets, while `GET /ready` returns 503. A cloud load balancer in front
  of the Service can keep sending requests to a terminating pod for up to about 20 seconds, and
  those requests would fail if workerd had already exited. After the delay, the pod hands off its
  objects to the other pods within 8 seconds, as `WORKERD_CLUSTER_STOP_TIMEOUT_MS` sets, and exits.
  The `terminationGracePeriodSeconds` value, 40, must cover both periods.
- **Rollouts.** A rollout starts one new pod at a time (`maxSurge: 1` and `maxUnavailable: 0`). It
  stops an old pod only after the new pod has been ready for 10 seconds (`minReadySeconds: 10`). A
  pod needs less time than that to hand off its objects, so only one pod hands off at a time. Each
  handoff starts at the end of its pod's 20-second shutdown delay, so two or three old pods are
  often terminating at the same time, while their handoffs still start more than 10 seconds apart.
- **Drains.** The PodDisruptionBudget lets a node drain or another eviction stop a workerd pod only
  while all workerd pods are ready. It doesn't wait for `minReadySeconds`, so after a stopped pod's
  replacement is ready, the next pod can be evicted while the first pod is still handing off its
  objects. The scheduler prefers to spread the pods across nodes, so each node holds few of them.
  Scaling down by more than one pod stops several pods at the same time. If you drain the node that
  runs the RustFS pod, the bucket is unavailable until the pod runs again on a node that its volume
  can attach to.
- **Calls to objects.** When an object moves, a `fetch()` call to it goes to its new pod. A
  JavaScript RPC call through a stub that reaches a stopping pod can fail once with a retryable
  error, and the stub's next call finds the object. `app/src/index.js` uses `fetch()`.
- **Network.** Anyone who can reach port 7000 and has the cluster key can call every service in the
  pod, and the traffic isn't encrypted. The NetworkPolicies let only the workerd pods connect to
  port 7000, and only the workerd pods and the bucket Job connect to RustFS. NetworkPolicies take
  effect only if the cluster's network plugin enforces them, and they never block connections from
  a pod's own node.

## Use a real bucket

To store the cluster's state in a replicated bucket instead of RustFS, do the following:

1. Remove the RustFS StatefulSet, Service, Job, and NetworkPolicy from `kubernetes.yaml`.
1. Configure the bucket for your provider:
   - For S3 or an S3-compatible service, such as R2, set `WORKERD_CLUSTER_BUCKET` to
     <code>s3://<var>BUCKET</var>/<var>PREFIX</var></code>. Set `AWS_ENDPOINT_URL` and `AWS_REGION`
     for your provider. Put the provider's access key ID and secret access key in the `access-key`
     and `secret-key` entries of the `bucket-credentials` Secret. All pods in a cluster must use the
     same region.

     Alternatively, mount a credentials file and set `AWS_CREDENTIALS_FILE` to its path. The file
     contains `AWS_ACCESS_KEY_ID=`, `AWS_SECRET_ACCESS_KEY=`, and, optionally,
     `AWS_SESSION_TOKEN=` lines. It doesn't use the `[default]` format of the AWS CLI. The pods
     reread the file when it changes.

   - For Cloud Storage, set `WORKERD_CLUSTER_BUCKET` to
     <code>gs://<var>BUCKET</var>/<var>PREFIX</var></code>, and remove the `AWS_*` variables. The
     pods then get tokens from GKE Workload Identity, or from the service account key file that
     `GOOGLE_APPLICATION_CREDENTIALS` names.

   Replace the following:
   - <code><var>BUCKET</var></code>: the bucket's name
   - <code><var>PREFIX</var></code>: the prefix of the cluster's objects in the bucket

> **Warning:** Anyone who can write to the bucket can publish code that every pod runs. Protect
> write access to the bucket as you protect the cluster key, and give write credentials only to the
> pods and to the people and systems that deploy the app.

## Roll out a new version

To roll out a new version of the app, do the following:

1. Build the app image with a new tag, such as `v2`. Use a new tag for every build: with
   `imagePullPolicy: IfNotPresent`, a node keeps using the image that it already has for a tag.
1. Make the image available to the cluster, as in [Run the sample](#run-the-sample).
1. Change the app image's tag in `kubernetes.yaml`.
1. Apply the manifest:

   ```sh
   kubectl apply -f samples/kubernetes/kubernetes.yaml
   ```

During a rollout, old and new pods run side by side, and a request for an object goes to the pod
that runs the object, whether that pod runs the old version or the new one. Each version must
therefore work with the previous version in the following ways:

- Keep the project's `name` and the storage keys of its Durable Object classes. A class's objects
  are stored under the project's name and the class's key, joined by a hyphen. The key is the class
  name when the class was created, and it doesn't change when you rename the class. A new project
  name or key starts with empty objects.
- Keep RPC compatible between versions. A call from an object on a new pod can reach an object on
  an old pod, and the reverse is also true. To add a method or field, add it in one version, and
  start using it in a later version. Remove it only after no pod runs a version that uses it.
- Keep stored data readable by the previous version. Objects move from old pods to new pods, so new
  code reads data that old code wrote. Old code can also read data that new code wrote: an object
  can move from a new pod back to an old pod during the rollout, for example when a new pod fails,
  and a rollback moves every object back.

## Limitations

The sample and cluster mode have the following limitations:

- Cluster mode is experimental and requires the `--experimental` flag.
- A cluster that you join with `--cluster-addr` uses the default for every setting except the
  address, the key, the bucket, and the stop timeout, which the environment variables set.
- While cluster mode is experimental, the protocol between pods can change from one workerd version
  to the next. A rollout that changes the workerd image can then fail requests that an old pod
  sends to a new pod, or the reverse, until the last old pod stops.
- Durable Objects with containers require a Docker engine, which a pod doesn't usually have.
- Pod IP addresses must be IPv4 addresses, because `--cluster-addr=$(POD_IP):7000` isn't a valid
  address for IPv6.
- Pod IP addresses must be private: in 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, or
  100.64.0.0/10. Because traffic between pods isn't encrypted, workerd refuses a public cluster
  address, and a Wrangler project can't set `unsafePublicAddress`. A Pod range of privately used
  public IP addresses, such as 240.0.0.0/4 on GKE, doesn't work.
