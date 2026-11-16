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
   docker ps -aq --filter name=workerd-container-client-test | xargs -r docker rm -f
   ```

4. Remove existing Docker image

   ```shell
   docker image rm cf-container-client-test
   ```

5. Run the test, which loads its images itself

   ```shell
   just stream-test //src/workerd/server/tests/container-client:container-client@
   ```
