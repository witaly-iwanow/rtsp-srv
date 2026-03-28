# Demo RTSP Server

A demo RTSP server written in C++ using FFmpeg under the hood.

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Run

```bash
./build/rtspsrv [media_directory] [[host:]port]
```

Defaults:

- `media_directory`: current working directory
- `host:port`: `0.0.0.0:554`

Examples:

```bash
./build/rtspsrv . 8554
sudo ./build/rtspsrv . 554
./build/rtspsrv ~/media [::]:8554
```
