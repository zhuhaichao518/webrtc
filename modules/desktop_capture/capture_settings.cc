#include "modules/desktop_capture/capture_settings.h"

//#ifdef WEBRTC_WIN
#include <Windowsx.h>
#include <windows.h>
#include <d3d11.h>
//#endif

namespace webrtc {

bool DecoderSettings::is_debugging_ = false;
std::atomic<bool> DecoderSettings::hardware_accelerated = false;

std::atomic<bool> DecoderSettings::encode_setted = false;


HWND hwnd = nullptr;

LRESULT CALLBACK WndProcT(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

bool DecoderSettings::showFrame(ID3D11Device* device, ID3D11Texture2D* texture, int width, int height){
  if (!hwnd){
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcT;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName = L"MyWindowClass";
    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, L"Window Registration Failed!", L"Error!",
                   MB_ICONEXCLAMATION | MB_OK);
        exit(1);
    }
    hwnd = CreateWindowEx(
        0,
        L"MyWindowClass",
        L"captured Image",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,width,height,NULL,NULL,NULL,NULL
    );

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
  }
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;

    ID3D11Texture2D* textureCopy = nullptr;
    HRESULT hr = device->CreateTexture2D(&desc, NULL, &textureCopy);
    if (FAILED(hr)) return false;

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    // 从原始纹理复制到副本
    context->CopyResource(textureCopy, texture);

    // 映射纹理以访问其数据
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    hr = context->Map(textureCopy, 0, D3D11_MAP_READ, 0, &mappedResource);
    if (FAILED(hr)) {
        textureCopy->Release();
        return false;
    }
    
    HDC hdc = GetDC(hwnd);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = mappedResource.RowPitch/4;
    bmi.bmiHeader.biHeight = -desc.Height; // 顶部-下部顺序
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(hdc, 0, 0, mappedResource.RowPitch/4, desc.Height, 0, 0, mappedResource.RowPitch/4, desc.Height, mappedResource.pData, &bmi, DIB_RGB_COLORS, SRCCOPY);

    // 清理
    ReleaseDC(hwnd, hdc);
    context->Unmap(textureCopy, 0);
    textureCopy->Release();

    context->Release();
    return true;
}

}  // namespace webrtc