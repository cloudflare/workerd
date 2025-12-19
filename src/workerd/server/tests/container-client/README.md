## container-client test

To run the tests:

1. Make sure Docker is installed and running correctly

   ```shell
   docker ps
   ```

2. Use correct Docker context

   ```shell
   docker context use default
   ```

3. Remove existing containers labeled as "cf-container-client-test"

   ```shell
   docker rm -f $(docker ps -aq --filter name=workerd-container-client-test --all)
   ```

4. Remove existing Docker image

   ```shell
   docker image rm cf-container-client-test
   ```

5. Build Docker images

   ```shell
   bazel run //images:load_all
   ```

6. Run the test

   ```shell
   just stream-test //src/workerd/server/tests/container-client
   ```
