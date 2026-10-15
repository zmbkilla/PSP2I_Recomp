#!/usr/bin/env python3
"""Validate video_decode against FFmpeg. Optional movie can be PMF or H.264.
Usage: python3 port/tests/check_video_decode.py path/to/video_decode [movie.pmf]
Requires ffmpeg on PATH. Generated test media contains no game assets.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(*args):
    subprocess.run([str(a) for a in args], check=True, timeout=120)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('probe', type=Path)
    parser.add_argument('movie', nargs='?', type=Path)
    args = parser.parse_args()
    probe = args.probe.resolve()
    with tempfile.TemporaryDirectory(prefix='psp2i-video-') as tmp:
        work = Path(tmp)
        synthetic = work / 'reordered.h264'
        run('ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
            'testsrc2=size=480x272:rate=30000/1001', '-frames:v', '90',
            '-c:v', 'libx264', '-bf', '3', '-f', 'h264', synthetic)
        inputs = [synthetic]
        if args.movie:
            movie = work / 'movie.h264'
            run('ffmpeg', '-v', 'error', '-y', '-i', args.movie.resolve(),
                '-map', '0:v:0', '-c:v', 'copy', '-f', 'h264', movie)
            inputs.append(movie)
        for source in inputs:
            actual, expected = work / 'actual.yuv', work / 'expected.yuv'
            run(probe, source, actual)
            run('ffmpeg', '-v', 'error', '-y', '-i', source, '-pix_fmt',
                'yuv420p', '-fps_mode', 'passthrough', '-f', 'rawvideo', expected)
            if actual.stat().st_size == 0:
                raise RuntimeError('Decoder produced no frames')
            with actual.open('rb') as a, expected.open('rb') as b:
                while True:
                    left, right = a.read(1048576), b.read(1048576)
                    if left != right:
                        raise RuntimeError(f'{source.name}: output differs from FFmpeg')
                    if not left:
                        break
            print(f'PASS {source.name}: all decoded pixels and frame count match FFmpeg')


if __name__ == '__main__':
    main()
