# syntax=docker/dockerfile:1

FROM alpine:3.22 AS build

RUN apk add --no-cache \
        build-base \
        ca-certificates \
        cmake \
        git \
        linux-headers \
        ninja

WORKDIR /src

COPY kcp /src/kcp
COPY shimakaze/CMakeLists.txt /src/shimakaze/CMakeLists.txt
COPY shimakaze/cmake /src/shimakaze/cmake
COPY shimakaze/src /src/shimakaze/src
COPY shimakaze/tests /src/shimakaze/tests

RUN cmake \
        -S /src/shimakaze \
        -B /src/build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DSHIMAKAZE_PREFER_LOCAL_KCP=ON \
        -DSHIMAKAZE_STATIC_LINK=ON \
    && cmake --build /src/build --parallel \
    && ctest --test-dir /src/build --output-on-failure \
    && cmake --install /src/build --prefix /out \
    && strip /out/bin/client /out/bin/server

RUN file /out/bin/client /out/bin/server \
    && ! readelf -l /out/bin/client | grep -q 'Requesting program interpreter' \
    && ! readelf -l /out/bin/server | grep -q 'Requesting program interpreter' \
    && ! readelf -d /out/bin/client | grep -q '(NEEDED)' \
    && ! readelf -d /out/bin/server | grep -q '(NEEDED)' \
    && /out/bin/client --version \
    && /out/bin/server --version

FROM scratch AS export
COPY --from=build /out /
