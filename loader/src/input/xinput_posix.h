// Linux XInput HLE: SDL2 keyboard/mouse/gamepad → Xbox port 0.
// Ports the Windows input/xinput.cpp to SDL2, keeping the same HLE table addresses and signatures.
#pragma once

#include "d3d/d3d_hooks_posix.h" // Hle

#include <stdint.h>

// The console's calling convention
#define XD3D_STDCALL __attribute__((stdcall))

namespace xinput {

// Xbox XINPUT_GAMEPAD (packed, 16 bytes + 8 analog buttons + packet = XState)
#pragma pack(push, 1)
struct XGamepad {
    uint16_t wButtons;
    uint8_t bAnalogButtons[8]; // A B X Y BLACK WHITE LTRIGGER RTRIGGER
    int16_t sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XState {
    uint32_t dwPacketNumber;
    XGamepad Gamepad;
};
#pragma pack(pop)

// Device handle the game receives from XInputOpen
struct Device {
    uint32_t magic;
    uint32_t port;
    uint32_t packet;
    XGamepad last;
};
constexpr uint32_t DEV_MAGIC = 0x44495858; // 'XXID'

// HLE entry points (same addresses as Windows invoice)
uint32_t XD3D_STDCALL XInitDevices(uint32_t dwPreallocTypeCount, void *PreallocTypes);
uint32_t XD3D_STDCALL XGetDeviceChanges(void *DeviceType, uint32_t *pdwInsertions, uint32_t *pdwRemovals);
void *XD3D_STDCALL XInputOpen(void *DeviceType, uint32_t dwPort, uint32_t dwSlot, void *pPollingParameters);
void XD3D_STDCALL XInputClose(void *h);
uint32_t XD3D_STDCALL XInputGetState(void *h, XState *pState);
uint32_t XD3D_STDCALL XInputSetState(void *h, uint8_t *pFeedback);

// The table for the HLE dispatcher
const xd3d::Hle *hle_input_table(size_t *count);

} // namespace xinput