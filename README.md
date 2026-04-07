# Demo RTSP Server

A simple RTSP server for serving local media files over RTP, based on `Asio` for async I/O and networking, and `libavformat`, `libavcodec`, and `libavutil` for in-process demuxing and RTP packetization. The server implements the RTSP control logic and streams requested files in a loop, and can serve multiple clients simultaneously.

The server supports video-only, audio-only, and audio+video inputs. It inspects containers through FFmpeg and exposes only tracks with codecs the RTP path supports.

The server handles `OPTIONS`, `DESCRIBE`, `SETUP`, `PLAY`, and `TEARDOWN`. SDP is generated in-process from RTP muxer contexts, and on `PLAY` the server opens the requested file itself and writes RTP packets directly to the client ports negotiated during `SETUP`.
Supported RTSP transport: UDP unicast RTP/RTCP with `client_port=` negotiation. Interleaved RTP over RTSP/TCP is not implemented.

Video codecs: HEVC, AVC, VP8, VP9 and MPEG-2 are streamed when present.
Audio codecs: Opus, AAC and MPEG Audio Layers 1/2/3 are streamed when present.
Unsupported tracks are omitted, so a file like `AV1 + Opus` is served as audio-only.

## Build

Linux / macOS (won't build on Windows - not sure people still use it)

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
sudo ./build/rtsp_server
./build/rtsp_server ./media 8554
./build/rtsp_server ./media [::]:8554
```

To play a file with VLC:

```bash
vlc rtsp://127.0.0.1:554/tos-480p.mp4
```
or
```bash
vlc rtsp://127.0.0.1:8554/tos-480p.mp4
```
depending on how you launched the server.

To play the same file with `ffplay`:

```bash
ffplay rtsp://127.0.0.1:554/tos-480p.mp4
```

Note: the `apt`-installed VLC on Debian / Ubuntu does not provide RTSP client support in a useful default configuration for this project. If you want to test with VLC there, either build VLC from source with `--enable-live555` or install VLC from `snap`.

## Docker

Build the image:

```bash
docker build -t rtsp-srv:ubuntu24.04 .
```

Run it with your media mounted into `./media` inside the container and use host networking. The image starts the server as `rtsp_server ./media 554`.

```bash
docker run --rm -it \
  --network host \
  -v "$PWD/media:/app/media:ro" \
  rtsp-srv:ubuntu24.04
```

Then open media through RTSP, for example:

```text
vlc rtsp://127.0.0.1:554/tos-480p.mp4
```
