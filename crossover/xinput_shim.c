// fakepad xinput shim (Windows DLL, built with mingw, loaded inside Wine).
// Reports the controller published by the native macOS helper to XInput callers
// (Steam and games). Reads state from Z:\tmp\fakepad_state; writes rumble to
// Z:\tmp\fakepad_rumble. No macOS-side hooking — pure Wine DLL override.
#include <windows.h>
#include <stdio.h>

#define STATE_PATH "Z:\\tmp\\fakepad_state"
#define RUMBLE_PATH "Z:\\tmp\\fakepad_rumble"

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XINPUT_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } XINPUT_VIBRATION;
typedef struct { BYTE Type, SubType; WORD Flags; XINPUT_GAMEPAD Gamepad; XINPUT_VIBRATION Vibration; } XINPUT_CAPABILITIES;

#pragma pack(push,1)
typedef struct { unsigned char connected,_pad; unsigned short buttons; unsigned char lt,rt; short lx,ly,rx,ry; unsigned int packet; } shared_t;
#pragma pack(pop)

static int read_state(shared_t *s){
    FILE *f = fopen(STATE_PATH, "rb");
    if (!f) return 0;
    int ok = fread(s, sizeof *s, 1, f) == 1;
    fclose(f);
    return ok && s->connected;
}

__declspec(dllexport) DWORD WINAPI XInputGetState(DWORD idx, XINPUT_STATE *st){
    if (idx != 0 || !st) return ERROR_DEVICE_NOT_CONNECTED;
    shared_t s;
    if (!read_state(&s)) return ERROR_DEVICE_NOT_CONNECTED;
    st->dwPacketNumber = s.packet;
    st->Gamepad.wButtons = s.buttons;
    st->Gamepad.bLeftTrigger = s.lt; st->Gamepad.bRightTrigger = s.rt;
    st->Gamepad.sThumbLX = s.lx; st->Gamepad.sThumbLY = s.ly;
    st->Gamepad.sThumbRX = s.rx; st->Gamepad.sThumbRY = s.ry;
    return ERROR_SUCCESS;
}
// Steam reads the Guide button via the undocumented ordinal 100 (XInputGetStateEx).
__declspec(dllexport) DWORD WINAPI XInputGetStateEx(DWORD idx, XINPUT_STATE *st){ return XInputGetState(idx, st); }

__declspec(dllexport) DWORD WINAPI XInputSetState(DWORD idx, XINPUT_VIBRATION *v){
    if (idx != 0) return ERROR_DEVICE_NOT_CONNECTED;
    if (v){ FILE *f=fopen(RUMBLE_PATH,"wb"); if(f){ unsigned char rb[2]={ v->wLeftMotorSpeed>>8, v->wRightMotorSpeed>>8 }; fwrite(rb,1,2,f); fclose(f);} }
    return ERROR_SUCCESS;
}
__declspec(dllexport) DWORD WINAPI XInputGetCapabilities(DWORD idx, DWORD flags, XINPUT_CAPABILITIES *c){
    shared_t s;
    if (idx != 0 || !read_state(&s)) return ERROR_DEVICE_NOT_CONNECTED;
    if (c){ ZeroMemory(c,sizeof *c); c->Type=1; c->SubType=1; /*GAMEPAD*/ c->Gamepad.wButtons=0xF3FF; c->Gamepad.bLeftTrigger=255; c->Gamepad.bRightTrigger=255;
            c->Gamepad.sThumbLX=(SHORT)0x7fff; c->Gamepad.sThumbLY=(SHORT)0x7fff; c->Gamepad.sThumbRX=(SHORT)0x7fff; c->Gamepad.sThumbRY=(SHORT)0x7fff;
            c->Vibration.wLeftMotorSpeed=255; c->Vibration.wRightMotorSpeed=255; }
    return ERROR_SUCCESS;
}
__declspec(dllexport) void WINAPI XInputEnable(BOOL e){ (void)e; }
__declspec(dllexport) DWORD WINAPI XInputGetBatteryInformation(DWORD i, BYTE t, void *b){ (void)i;(void)t;(void)b; return ERROR_DEVICE_NOT_CONNECTED; }
__declspec(dllexport) DWORD WINAPI XInputGetKeystroke(DWORD i, DWORD r, void *k){ (void)i;(void)r;(void)k; return ERROR_DEVICE_NOT_CONNECTED; }
__declspec(dllexport) DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD i, void *a, void *b){ (void)i;(void)a;(void)b; return ERROR_DEVICE_NOT_CONNECTED; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID x){ (void)h;(void)r;(void)x; return TRUE; }
