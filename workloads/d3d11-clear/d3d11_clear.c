// SPDX-License-Identifier: GPL-3.0-or-later
// Original BetterXen workload (issue #365, first step): Direct3D 11 clear and readback.
// Creates a hardware device, clears a 64x64 RGBA8 render target to (0.25, 0.5, 0.75, 1.0),
// copies it to a staging texture, maps it, and checks every pixel against the expected bytes
// (64, 128, 191, 255; 0.25*255 = 63.75 rounds to 64, 0.5*255 = 127.5 to 128, 0.75*255 = 191.25
// to 191, under D3D11's round-to-nearest conversion). Exit codes: 0 pass, 1 wrong pixel,
// 2 no device (reported, not a pass), 3 other API failure.
#define COBJMACROS
#include <d3d11.h>
#include <stdio.h>

int main(void) {
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                   D3D11_SDK_VERSION, &dev, &got, &ctx);
    if (FAILED(hr)) { printf("NO DEVICE hr=0x%08lx\n", (unsigned long)hr); return 2; }
    printf("device ok feature_level=0x%x\n", (unsigned)got);

    D3D11_TEXTURE2D_DESC d = {0};
    d.Width = 64; d.Height = 64; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = NULL, *st = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &rt))) { printf("FAIL create rt\n"); return 3; }
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st))) { printf("FAIL create staging\n"); return 3; }
    ID3D11RenderTargetView *rtv = NULL;
    if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv))) {
        printf("FAIL create rtv\n"); return 3;
    }
    const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, color);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        printf("FAIL map\n"); return 3;
    }
    int bad = 0;
    for (UINT y = 0; y < 64; y++) {
        const unsigned char *row = (const unsigned char *)m.pData + (size_t)y * m.RowPitch;
        for (UINT x = 0; x < 64; x++) {
            const unsigned char *p = row + x * 4;
            if (p[0] != 64 || p[1] != 128 || p[2] != 191 || p[3] != 255) {
                if (bad < 3) printf("pixel (%u,%u) = %u %u %u %u\n", x, y, p[0], p[1], p[2], p[3]);
                bad++;
            }
        }
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    if (bad) { printf("FAIL %d wrong pixels\n", bad); return 1; }
    printf("PASS 4096 pixels\n");
    return 0;
}
