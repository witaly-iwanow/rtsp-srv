FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        cmake \
        libavcodec-dev \
        libavformat-dev \
        libavutil-dev \
        libasio-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt ./
COPY src ./src

RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libavcodec60 \
        libavformat60 \
        libavutil58 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

RUN mkdir -p /app/media

COPY --from=build /build/rtsp_server /usr/local/bin/rtsp_server

ENTRYPOINT ["/usr/local/bin/rtsp_server"]
CMD ["./media", "554"]
