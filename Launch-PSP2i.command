#!/bin/zsh
set -e
cd "${0:A:h}"
export PSP2I_DATA="$PWD"
exec ./port/build-macos/psp2i
