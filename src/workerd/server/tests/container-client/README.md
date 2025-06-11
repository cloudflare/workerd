## container-client test

This test is excluded from test runner. In order to run the tests, please read the following steps:

- Make sure docker is installed and running correctly

```shell
docker ps
```

- Remove existing containers labeled as "cf-container-client-test"

```shell
docker rm -f $(docker ps -aq --filter name=workerd-container-client-test --all)
```

- Remove existing docker image

```shell
docker image rm cf-container-client-test
```

- Build docker image

```shell
docker build -t "cf-container-client-test" src/workerd/server/tests/container-client
```

- Run the test

```shell
just stream-test //src/workerd/server/tests/container-client:test
```