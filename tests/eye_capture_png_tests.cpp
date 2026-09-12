#include "../src/openxr/EyeCapturePng.h"
#include <array>
#include <string>
#include <cstdlib>
void check(bool ok){if(!ok)std::abort();}
int main(){
    check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)));
    wchar_t temp[MAX_PATH]{};check(GetTempPathW(MAX_PATH,temp)!=0);
    auto path=std::filesystem::path(temp)/(L"kharvox-png-test-"+std::to_wstring(GetCurrentProcessId())+L".png");
    std::array<BYTE,16> pixels{0,0,255,255,0,255,0,255,255,0,0,255,17,31,63,255};
    check(kharvox::writeEyePng(path,2,2,pixels.data()));
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        Microsoft::WRL::ComPtr<IWICFormatConverter> convert;
        check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))));
        check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder)));
        check(SUCCEEDED(decoder->GetFrame(0,&frame)));
        UINT w{},h{};check(SUCCEEDED(frame->GetSize(&w,&h))&&w==2&&h==2);
        check(SUCCEEDED(factory->CreateFormatConverter(&convert)));
        check(SUCCEEDED(convert->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)));
        std::array<BYTE,16> result{};check(SUCCEEDED(convert->CopyPixels(nullptr,8,16,result.data())));
        check(result==pixels); // No channel swap, vertical flip or alpha loss.
    }
    std::filesystem::remove(path);
    CoUninitialize();
}
