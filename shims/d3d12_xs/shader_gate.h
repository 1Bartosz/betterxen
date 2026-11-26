#pragma once

/*
 * M2 Shader Format Gate (#466): classifies every shader handed to Create*PipelineState through
 * bx_shader_bridge.dll (abi/include/bx_shader_bridge.h, the Windows build of dxbc-accc, #445)
 * and decides what the host device receives. See docs/adr/0008-m2-shader-format-gate.md.
 */

#include <windows.h>
#include <d3d12.h>

#include <cstddef>
#include <cstdint>

class BxShaderGate {
public:
    BxShaderGate() = default;
    ~BxShaderGate();
    BxShaderGate(const BxShaderGate&) = delete;
    BxShaderGate& operator=(const BxShaderGate&) = delete;

    /*
     * Logs one stage's classification (`shader_gate` event) and sets `out` to the bytecode the
     * host gets: the bridge's prepared DXIL copy for DXIL and dual containers, or `in` unchanged
     * for legacy DXBC, unclassifiable input, or when the bridge is missing (logged once as
     * `shader_bridge_missing`). Returns E_NOTIMPL for an ACCC-only container, which has no
     * DXIL for D3DMetal until M3 Branch D2. Copies live until this object is destroyed.
     */
    HRESULT admit(const char* pso_kind, const char* stage, const D3D12_SHADER_BYTECODE& in,
                  D3D12_SHADER_BYTECODE& out);

private:
    static constexpr size_t kMaxCopies = 5; /* VS, PS, DS, HS, GS */
    uint8_t* copies_[kMaxCopies] = {};
    size_t copy_sizes_[kMaxCopies] = {};
    size_t copy_count_ = 0;
};
