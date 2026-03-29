# Demo RTSP Server

A C++ RTSP server for serving local media files over RTP. Start it with a media directory and a bind address, then open `rtsp://host:port/<filename>` in a client such as `ffplay` or VLC. The server implements the RTSP control plane itself and streams the requested file in a loop. The number of clients connected simultaneously is not limited - just wait for it to run out of RAM/CPU. The server supports video-only, audio-only, and audio+video inputs. In practice it can handle whatever the local `ffprobe`/`ffmpeg` build can demux and process; the current limitation is that file discovery is gated by a small extension whitelist in [src/rtsp_server.cpp](/home/vitaly/tmp/rtsp-srv/git/src/rtsp_server.cpp#L24), feel free to edit it.

The server handles `OPTIONS`, `DESCRIBE`, `SETUP`, and `PLAY`, generates SDP using `ffmpeg`, and on `PLAY` launches `ffmpeg` to send RTP directly to the client ports negotiated during `SETUP`.
Supported RTSP transport: UDP unicast RTP/RTCP with `client_port=` negotiation. Interleaved RTP over RTSP/TCP is not implemented (FFmpeg's RTP mux limitation).

Video codecs: HEVC, AVC, VP8, VP9 and MPEG-2 are sent as-is; other video codecs are transcoded to AVC as a precaution. AV1 can be added to the list with FFmpeg 7.1 or later, just add `-bsf:v av1_frame_split -strict experimental` for this case.
Audio codecs: Opus, AAC and MPEG Audio Layers 1/2/3 are sent as-is; other audio codecs are transcoded to Opus.

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
./build/rtsp_server . 8554
./build/rtsp_server ~/media [::]:8554
```

To play a file with VLC:

```bash
vlc rtsp://127.0.0.1:554/tos-480p.mp4
```

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
