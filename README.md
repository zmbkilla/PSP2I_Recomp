<h1 align="center"> Phantasy Star Portable 2 Infinity Recomp</h1>

<p align="center">
  <img src="https://i0.wp.com/bumped.org/psublog/wp-content/uploads/2010/08/Phantasy-star-portable-2-infinity.png?resize=338%2C213" alt="Icon">
</p>

A recompilation project for **Phantasy Star Portable 2 Infinity (PSP2i)**, bringing the game to a native PC environment while preserving the original game's functionality and feel.

## AI Disclosure

This project makes **heavy use of AI-assisted development, primarily Claude**, for code analysis, reverse engineering assistance, debugging, implementation, and development tooling.

AI-generated code and suggestions are reviewed, tested, and integrated as part of the development process.

## About

**PSP2I_Recomp** is a recompilation project for *Phantasy Star Portable 2 Infinity*.

The current goal is to produce a native PC build of the game while maintaining compatibility with the original game logic and improving the experience with modern PC features.

This project is based on the output of the PSP recompilation toolchain and ongoing community reverse-engineering efforts.

## Capabilities

Current and planned capabilities include:

* Native PC executable
* Direct3D 11 rendering
* SDL3 input support
* Keyboard and controller input
* Right-stick camera control
* Save/load functionality
* Optional 60 FPS mode
* Original 30 FPS mode
* Infrastructure handling (Head to [psp2i discord](https://discord.gg/FeGvdBkrGa) for more info)

> This project is actively being developed. Capabilities and compatibility may change as development progresses.

## Building

### Requirements

- Windows
- Visual Studio with C++ development tools
- CMake
- Git
- FFMPEG 9.0.1
- SDL3 3.4.18
- A legally obtained copy of *Phantasy Star Portable 2 Infinity*

### Build

Clone the repository and the `psprecomp` dependency:

```bash
git clone https://github.com/zmbkilla/PSP2I_Recomp.git
cd PSP2I_Recomp
git clone https://github.com/zmbkilla/psprecomp.git
```

Build `allegrexrecomp`:

```bash
cmake --build psprecomp/build --config Release --target allegrexrecomp
```

Generate the PSP2i recompilation code from the game's `EBOOT.BIN`:

```bash
cd port

../psprecomp/build/tools/allegrexrecomp/Release/allegrexrecomp.exe emit ../FinalBuild/EBOOT.BIN gen recomp --hooks hooks.txt --fixes fixes.txt

cd ..
```

Build the PSP2i executable:

```bash
cmake --build port/build --config Release --parallel --target psp2i
```

The resulting executable is:

```text
port/build/Release/psp2i.exe
```

Place the SDL3 and FFMPEG binaries `avcodec-63, avutil-61, swresample-7` into executable's directory 

### Optional Checks

The following commands can be used to verify that the expected PSP2i hooks are present:

```bash
grep -c "0x08D3F3B4\|0x08D3F418\|0x08D4C5C0\|0x08D3F470\|0x08D4C604\|0x08B8D840" port/hooks.txt
grep -c "hook_hang_fatal\|hook_game_report" port/host/gamelog.c
```

These commands only verify the source files and do not modify the project.

### Notes

The recompilation step requires a game `EBOOT.BIN` obtained from a legally owned copy of the game. The original game data is not included in this repository.

`allegrexrecomp` must be built before running the `emit` command because the `emit` step uses the generated `allegrexrecomp.exe`.

The `emit` command reads `FinalBuild/EBOOT.BIN` and generates the recompilation sources in `port/gen/`, which are then compiled as part of the `psp2i` target.



## Credits

### PSP Recompilation Project

This project builds upon the work and tooling provided by **psprecomp**, which makes the recompilation of PSP software possible.

### FFmpeg

Audio functionality makes use of **FFmpeg**. pmf is not handled yet

### PSP2i Community

Thanks to the **Phantasy Star Portable 2 Infinity community** for research and documentation of the game and providing mods like the fan translated patch.
