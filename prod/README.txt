Phantasy Star Portable 2 Infinity (NPJH50332) - PC port
=======================================================

This folder is the community build. It contains no game files: you need your
own copy of the game.

Setup
-----
Put these next to psp2i.exe:

  GameData\disc\         your game disc, extracted (the folder that contains
                         PSP_GAME\PARAM.SFO, PSP_GAME\USRDIR, PSP_GAME\SYSDIR).
			 Ex. C:\psp2i\GameData\disc\PSP_GAME
  GameData\flash\font\   optional: the PSP firmware fonts (jpn0.pgf, kr0.pgf,
                         ltn0.pgf ... ltn15.pgf) from your PSP's flash0:/font.
                         Without them the game's text uses a Windows font; other
                         fonts can go in a fonts\ folder next to psp2i.exe and
                         be picked in the Engine Menu (FONT).

The game EBOOT is read from GameData\disc\PSP_GAME\SYSDIR\EBOOT.BIN. It must be
decrypted (a plain ELF, not ~PSP). If you keep it elsewhere, put it next to
psp2i.exe as EBOOT.BIN; that copy is used first. The revival server's patched
EBOOT also works.

Already included here: psp2i.exe, SDL3.dll (controllers and sound), and
psp2i_atrac.dll for the game's music: FFmpeg's ATRAC3plus decoder as a separate
DLL under the LGPL 2.1 (licenses\ has the licence and where its source is).

Running
-------
Double-click psp2i.exe. If anything above is missing or unusable, it shows a
message listing exactly what, then closes.

The only command-line option is --console: it opens a console window showing
the engine's messages (e.g. a shortcut to "psp2i.exe --console"). Without it no
console appears. The logs are written either way; please include them with
bug reports.

Created on first run, next to psp2i.exe:
  GameData\ms\           the memory stick (saves go in GameData\ms\PSP\SAVEDATA)
  logs\                  game_log.txt, online_log.txt, and after a crash
                         crash_log.txt and crash_dump.dmp
  psp2i_online.ini       online settings

Controls
--------
Engine Menu: Press F1 (keyboard) or right stick (controller) ingame [tested using PS5 controller]