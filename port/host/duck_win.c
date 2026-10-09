/* Attenuation: is another program playing sound? (Discord, a video in the
 * browser, ...) -- for audio_sdl.c, which then turns the game down.
 *
 * Windows keeps one audio session per program on each output device, with a
 * peak meter. A background thread looks at the default output device's
 * sessions every 250 ms: any session that is not this process and not the
 * system sounds, with a peak above a whisper, counts as "other audio". Only
 * the time it was last heard is shared with the audio thread (one atomic
 * value); the game thread never waits on Windows audio. */
#include "duck.h"

#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <stdio.h>

static const GUID CLSID_MMDeviceEnumerator_ = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID IID_IMMDeviceEnumerator_  = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID IID_IAudioSessionManager2_ = { 0x77AA99A0, 0x1BD6, 0x484F, { 0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F } };
static const GUID IID_IAudioSessionControl2_ = { 0xBFB7FF88, 0x7239, 0x4FC9, { 0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D } };
static const GUID IID_IAudioMeterInformation_ = { 0xC02216F6, 0x8C67, 0x4B5B, { 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64 } };

#define PEAK_THRESHOLD 0.02f              /* about -34 dBFS: silence and hiss do not count */

static volatile LONG64 g_heard_ms;         /* GetTickCount64 when other audio was last heard */
static volatile LONG   g_running;

/* 1 if any other program is making sound right now on the default output. */
static int others_playing(IMMDeviceEnumerator *en) {
    IMMDevice *dev = NULL;
    IAudioSessionManager2 *mgr = NULL;
    IAudioSessionEnumerator *list = NULL;
    int found = 0;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) return 0;
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioSessionManager2_, CLSCTX_ALL, NULL, (void **)&mgr))) goto done;
    if (FAILED(IAudioSessionManager2_GetSessionEnumerator(mgr, &list))) goto done;
    int n = 0;
    IAudioSessionEnumerator_GetCount(list, &n);
    const DWORD self = GetCurrentProcessId();
    for (int i = 0; i < n && !found; i++) {
        IAudioSessionControl *ctl = NULL;
        IAudioSessionControl2 *ctl2 = NULL;
        IAudioMeterInformation *meter = NULL;
        if (FAILED(IAudioSessionEnumerator_GetSession(list, i, &ctl))) continue;
        if (SUCCEEDED(IAudioSessionControl_QueryInterface(ctl, &IID_IAudioSessionControl2_, (void **)&ctl2))) {
            DWORD pid = 0;
            IAudioSessionControl2_GetProcessId(ctl2, &pid);
            const int system = IAudioSessionControl2_IsSystemSoundsSession(ctl2) == S_OK;
            if (pid != self && pid != 0 && !system &&
                SUCCEEDED(IAudioSessionControl_QueryInterface(ctl, &IID_IAudioMeterInformation_, (void **)&meter))) {
                float peak = 0.0f;
                if (SUCCEEDED(IAudioMeterInformation_GetPeakValue(meter, &peak)) && peak > PEAK_THRESHOLD) found = 1;
                IAudioMeterInformation_Release(meter);
            }
            IAudioSessionControl2_Release(ctl2);
        }
        IAudioSessionControl_Release(ctl);
    }
done:
    if (list) IAudioSessionEnumerator_Release(list);
    if (mgr) IAudioSessionManager2_Release(mgr);
    if (dev) IMMDevice_Release(dev);
    return found;
}

static DWORD WINAPI watch(LPVOID unused) {
    (void)unused;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 0;
    IMMDeviceEnumerator *en = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator_, (void **)&en))) {
        fprintf(stderr, "audio: cannot watch other programs' sound; attenuation off\n");
        CoUninitialize();
        return 0;
    }
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        if (others_playing(en)) InterlockedExchange64(&g_heard_ms, (LONG64)GetTickCount64());
        Sleep(250);
    }
    IMMDeviceEnumerator_Release(en);
    CoUninitialize();
    return 0;
}

void duck_start(void) {
    if (InterlockedExchange(&g_running, 1)) return;
    HANDLE t = CreateThread(NULL, 64 * 1024, watch, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&g_running, 0);
}

void duck_stop(void) { InterlockedExchange(&g_running, 0); }

int duck_others_heard_within(unsigned ms) {
    if (!InterlockedCompareExchange(&g_running, 1, 1)) return 0;
    const LONG64 t = InterlockedCompareExchange64(&g_heard_ms, 0, 0);
    return t && (LONG64)GetTickCount64() - t < (LONG64)ms;
}

#else
void duck_start(void) { }
void duck_stop(void) { }
int duck_others_heard_within(unsigned ms) { (void)ms; return 0; }
#endif
