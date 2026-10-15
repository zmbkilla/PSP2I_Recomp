# Native Apple Silicon build

This experimental macOS target uses the SDL host shared with Android. It is
separate from the Windows host and does not provide all of its features.

From the repository root:

```sh
brew install cmake ninja sdl3 ffmpeg
./build_macos.sh
./Launch-PSP2i.command
```

The build script initializes psprecomp, applies the included Apple ARM64 fiber
patch if needed, generates C from your decrypted EBOOT.BIN, and builds the
executable at port/build-macos/psp2i. Four compiler jobs limit peak memory use.
The patch is also applied in the local psprecomp submodule; preserve that change.

Keep EBOOT.BIN and game.iso in the repository root. The launcher sets PSP2I_DATA
and the working directory to that folder. psp2i.ini contains:

```ini
iso=game.iso
fps=30
touch=off
```

Place your PSP fonts or compatible replacement fonts in GameData/flash/font.
Saves go under
GameData/ms; the runtime log is psp2i_log.txt in the repository root.

Keyboard controls (click the game window first):

| Keys | PSP control |
| --- | --- |
| Arrow keys | D-pad |
| I / J / K / L | Analog up / left / down / right |
| X / Z | Cross / Circle |
| A / S | Square / Triangle |
| Q / E | L / R |
| Return / Right Shift | Start / Select |

SDL-compatible controllers are also supported, including right-stick camera.
Close the game window to quit.

Movie video uses the system FFmpeg libraries (libavcodec, libavutil, and libswscale).
The runtime supplies H.264 access units; the host returns decoded YUV420P
frames with their presentation timestamps. Decoder buffers are drained at EOF.
To build without video decoding, configure with `-DPSP2I_LIBAV=OFF`; movies
then retain the previous black-video behavior. Windows continues to use its
existing OpenH264 backend. Android does not enable this optional backend.

Build and run the standalone decoder comparison (no game code executed):

```sh
cmake --build port/build-macos --target video_decode
python3 port/tests/check_video_decode.py port/build-macos/video_decode
# Optionally compare a legally obtained PMF too:
python3 port/tests/check_video_decode.py port/build-macos/video_decode /path/to/movie.pmf
```

The test generates 90 frames with B-frame reordering and compares all decoded
YUV pixels and frame counts against FFmpeg, including EOF drain. The probe
feeds Annex-B input through a parser in 2,048-byte chunks. It also accepts an
optional PMF, extracting its video with FFmpeg before the comparison. This
checks the codec adapter; it does not independently validate the runtime's
PSMF demultiplexer, presentation timing, or audio/video synchronization.

The decoder approach was informed by
[kion/psp2i-recomp](https://psp2i.dev/kion/psp2i-recomp), revision
`e7d140fde6836e2233028b160e2e47792074c179`, especially its FFmpeg decoder and
frame-comparison harness. This implementation uses psprecomp's existing codec
interface and demultiplexer.

This SDL host still lacks the Windows settings menu, infrastructure online
support, and encrypted save compatibility with PSP/PPSSPP/Windows saves.

Full-range cutscenes (YUVJ420P, including the Continue path) are normalized to
studio-range YUV420P before the runtime's BT.601 color conversion. The decoder
comparison also covers 90 full-range frames with B-frame reordering.
