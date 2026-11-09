# MiniVectorDB evaluation image.
#
# Stage 1 builds the database from source with the distribution toolchain.
# Stage 2 keeps only the binary: the program links libstdc++/libgcc statically,
# so the runtime image needs nothing beyond glibc from the base image.

FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ cmake make ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY minivectordb/ /src/minivectordb/

RUN cmake -S /src/minivectordb -B /src/build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /src/build -j"$(nproc)"

FROM ubuntu:24.04

RUN mkdir -p /data
COPY --from=build /src/build/minivectordb /usr/local/bin/minivectordb

VOLUME ["/data"]
WORKDIR /app

# The evaluator keeps stdin open and streams SQL statements; results go to stdout
# and logs go to stderr. No network listener, no exposed port.
ENTRYPOINT ["/usr/local/bin/minivectordb"]
CMD ["--data-dir", "/data"]
