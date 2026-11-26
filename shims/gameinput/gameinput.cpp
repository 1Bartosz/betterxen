#include <windows.h>

#include "bx_log.h"

/* Track E (M1-E2): GameInput.dll ordinal 1 (GameInputCreate) shim. */

extern "C" HRESULT WINAPI GameInputCreate(void** out_game_input) {
    BX_STUB_LOG("GameInput.dll", "GameInputCreate@1");
    if (out_game_input) {
        *out_game_input = nullptr;
    }
    return E_NOTIMPL;
}
