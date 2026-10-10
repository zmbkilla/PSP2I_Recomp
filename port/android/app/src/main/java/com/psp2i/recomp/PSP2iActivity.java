package com.psp2i.recomp;

import org.libsdl.app.SDLActivity;

/* SDL's activity runs main() from libmain.so (port/host/sdl_main.c). */
public class PSP2iActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
