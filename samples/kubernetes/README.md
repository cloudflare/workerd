# A workerd cluster on Kubernetes

This sample uses workerd's experimental cluster mode to run an app with Durable Objects on three
workerd pods. The pods share an S3-compatible bucket. `workerd compile` compiles the app into the
container image, and Kubernetes rolls out new image versions.

Each Durable Object runs on one pod at a time. Any pod can accept a request for any object. It sends
the request to the pod that runs the object. When Kubernetes stops a pod for a rollout, a
scale-down, or a node drain, the pod hands its objects to the other pods before it exits. Requests
keep working.

## Files

- `counter.js`: runs the app. `GET /<name>` adds one to the Durable Object counter named `<name>`.
  It reports which pod accepted the request and which pod runs the object.
- `config.capnp`: defines the config that each pod runs. It holds no secrets. The cluster key and
  bucket come from environment variables.
- `bucket.capnp`: provides a stand-in bucket for trying the sample. It uses the fake S3 from
  workerd's cluster tests. Do not use it in production.
- `Dockerfile`: builds one image with both configs compiled into binaries.
- `kubernetes.yaml`: defines the workerd Deployment, its Service, a PodDisruptionBudget, a
  NetworkPolicy, and the demo bucket.

## Try it

You need a Kubernetes cluster, such as [kind](https://kind.sigs.k8s.io/). You also need a workerd
version that reads the cluster key from `WORKERD_CLUSTER_KEY_FILE`; the image build stops if
workerd is too old. Until a release has it, build workerd from source on Linux, for the image's
architecture, and pass the binary to `docker build` as the Dockerfile describes.

From the repository root, build the image:

```sh
docker build -f samples/kubernetes/Dockerfile -t workerd-kubernetes-sample .
```

Make the image available to the cluster. For example, run
`kind load docker-image workerd-kubernetes-sample`. You can instead push it to a registry and
change `image:` in `kubernetes.yaml`. Then create the cluster key and start everything:

```sh
kubectl create secret generic workerd-cluster --from-literal=key="$(openssl rand -hex 32)"
kubectl apply -f samples/kubernetes/kubernetes.yaml
kubectl rollout status deployment/workerd
```

The workerd pods may restart once while the demo bucket starts. A pod exits if it cannot read the
bucket at startup, and Kubernetes starts it again.

Send some requests from inside the cluster:

```sh
kubectl run client --image=curlimages/curl --restart=Never --command -- sleep infinity
kubectl exec client -- sh -c 'for i in 1 2 3 4 5; do curl -s http://workerd/alice; echo; done'
```

The count goes up by one each time. `objectPod` stays the same, whichever pod accepted the request.
Now keep sending requests while Kubernetes replaces every pod:

```sh
kubectl exec client -- sh -c 'while true; do curl -s -w " %{http_code}\n" http://workerd/alice; done' &
kubectl rollout restart deployment/workerd
```

No request fails, and no count is skipped or repeated. `objectPod` moves to the new pods.

## How it fits together

- **Key and bucket.** `config.capnp` leaves `cluster.key` and `cluster.bucket` empty. Each pod reads
  them from `WORKERD_CLUSTER_KEY_FILE` (the Secret, mounted as a file) and
  `WORKERD_CLUSTER_BUCKET`. This lets the same image join any cluster.
- **Addresses.** Pods listen for each other on port 7000. Each pod advertises its pod IP with
  `--cluster-advertise=$(POD_IP):7000`.
- **Readiness.** `GET /ready` on port 7000 answers 200 while the pod serves. It answers 503 while
  the pod starts or stops, so the Service stops sending requests to it while it stops.
- **Stopping.** On SIGTERM, a pod hands its objects to the other pods within `stopTimeoutMs` and
  then exits. The default is 8 seconds. `terminationGracePeriodSeconds` must be longer.
- **One pod at a time.** Rollouts and node drains stop one pod at a time. Rollouts use
  `maxSurge: 1`, `maxUnavailable: 0`, and `minReadySeconds: 10`. The PodDisruptionBudget limits
  drains to one pod, and the scheduler prefers to spread the pods over the nodes. Scaling down by
  more than one pod stops several pods at once.
- **`fetch()` and JS RPC.** A `fetch()` call to an object that moves is sent on to its new pod.
  A JS RPC call that reaches a stopping pod through a stub can fail once with a retryable error.
  The stub's next call finds the object. `counter.js` uses `fetch()`.
- **Local storage.** An `emptyDir` volume stores each pod's copy of the objects it runs. The bucket
  holds the authoritative copy, so pods need no persistent volumes.
- **Network.** The NetworkPolicy lets only the workerd pods connect to port 7000. Anyone who
  reaches that port with the cluster key can call every service in the pod. The traffic is not
  encrypted.

## Using a real bucket

Remove the `fake-s3` Deployment and Service from `kubernetes.yaml`. Set `WORKERD_CLUSTER_BUCKET`
to `s3://<bucket>/<prefix>`. Set `AWS_ENDPOINT_URL` and `AWS_REGION` for your provider, such as R2
or S3. Take `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY` from a Secret. You can instead mount a
credentials file and set `AWS_CREDENTIALS_FILE`. All pods of a cluster must be in one region.

## Rolling out a new version

During a rollout, old and new pods run side by side. A request for an object goes to the pod that
runs it, whether old or new. Each version must therefore work alongside the previous version:

- Keep Durable Object class names and their `uniqueKey`s. A `uniqueKey` names the objects' storage.
  A new one therefore starts with empty objects.
- Keep RPC compatible between versions. A call from an object on a new pod can reach an object on
  an old pod, and the other way around. Add a method or field in one version and start using it
  in a later one. Remove it only after no pod runs a version that uses it.
- Objects move from old pods to new pods, so new code reads data that old code wrote. Old code
  can read data that new code wrote, too. An object can move from a new pod back to an old one
  during the rollout, for example when a new pod fails, and a rollback moves every object back.
  Keep stored data readable by the previous version.

## Limitations

- Cluster mode is experimental and needs `--experimental`.
- Durable Objects with containers need a Docker engine, which a pod does not normally have.
- Pod IPs must be IPv4. `--cluster-advertise=$(POD_IP):7000` and the listen address
  `0.0.0.0:7000` do not handle IPv6.
