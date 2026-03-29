# Demo RTSP Server

A demo RTSP server written in C++ using FFmpeg under the hood.

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Run

```bash
./build/rtsp_server [media_directory] [[host:]port]
```

Defaults:

- `media_directory`: current working directory
- `host:port`: `0.0.0.0:554`

Examples:

```bash
./build/rtsp_server . 8554
sudo ./build/rtsp_server . 554
./build/rtsp_server ~/media [::]:8554
```

## Docker

Build the image:

```bash
docker build -t rtsp-srv:ubuntu24.04 .
```

Run it with a media directory mounted into `/media`. The container serves from `.` inside `/media`, matching the native default:

```bash
docker run --rm -it \
  -p 554:554 \
  -v "$PWD:/media:ro" \
  rtsp-srv:ubuntu24.04
```

Then open media through RTSP, for example:

```text
rtsp://localhost:554/meridian-480.mp4
```
