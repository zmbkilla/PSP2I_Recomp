Phantasy Star Portable 2 Infinity (NPJH50332) - PC port
=======================================================

This folder is the community build. It contains no game files: you need your
own copy of the game.

Setup
-----
Put these next to psp2i.exe:

  GameData\disc\         your game disc, extracted (the folder that contains
                         PSP_GAME\PARAM.SFO, PSP_GAME\USRDIR, PSP_GAME\SYSDIR)
  GameData\flash\font\   the PSP firmware fonts (jpn0.pgf, kr0.pgf,
                         ltn0.pgf ... ltn15.pgf) from your PSP's flash0:/font

The game EBOOT is read from GameData\disc\PSP_GAME\SYSDIR\EBOOT.BIN. It must be
decrypted (a plain ELF, not ~PSP). If you keep it elsewhere, put it next to
psp2i.exe as EBOOT.BIN; that copy is used first. The revival server's patched
EBOOT also works.

Already included here: psp2i.exe, SDL3.dll (controllers and sound), and the
FFmpeg DLLs (avcodec, avutil, swresample) for the game's music.

Running
-------
Double-click psp2i.exe. It takes no command-line options. If anything above is
missing or unusable, it shows a message listing exactly what, then closes.

Created on first run, next to psp2i.exe:
  GameData\ms\           the memory stick (saves go in GameData\ms\PSP\SAVEDATA)
  psp2i_online.ini       online settings
