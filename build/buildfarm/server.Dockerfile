FROM bazelbuild/buildfarm-server:2.10.2

RUN rm /app/build_buildfarm/config.minimal.yml
COPY config.yml /app/build_buildfarm/config.minimal.yml
