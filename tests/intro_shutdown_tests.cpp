// Exercise the production teardown without starting a headset session.
#define wWinMain unusedIntroEntry
#include "../src/intro/VrGameIntro.cpp"
#undef wWinMain
#include <iostream>

static std::vector<int> calls;
static HANDLE completion;
static IntroApp* current;
static bool failSession;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static void record(int stage){
    require(WaitForSingleObject(completion,0)==WAIT_TIMEOUT,"handoff signalled during XR teardown");
    require(current->preview.handle()==nullptr,"desktop resources survive into XR teardown");
    require(current->eyes[0].targets.empty(),"eye render target still referenced");
    calls.push_back(stage);
}
static XrResult XRAPI_PTR destroySwapchain(XrSwapchain){record(1);return XR_SUCCESS;}
static XrResult XRAPI_PTR destroySpace(XrSpace){record(2);return XR_SUCCESS;}
static XrResult XRAPI_PTR destroySession(XrSession){record(3);return failSession?XR_ERROR_RUNTIME_FAILURE:XR_SUCCESS;}
static XrResult XRAPI_PTR destroyActions(XrActionSet){record(4);return XR_SUCCESS;}
static XrResult XRAPI_PTR destroyInstance(XrInstance){record(5);return XR_SUCCESS;}
int main(){try{
    bool missing=false;try{checkHeadsetSystem(XR_ERROR_FORM_FACTOR_UNAVAILABLE);}catch(const IntroHeadsetUnavailable&){missing=true;}
    require(missing&&IntroHeadsetUnavailable::exitCode==0x4b480002,"missing headset status not propagated");
    bool generic=false;try{checkHeadsetSystem(XR_ERROR_RUNTIME_FAILURE);}catch(const IntroHeadsetUnavailable&){throw;}catch(const std::runtime_error&){generic=true;}
    require(generic,"runtime failure mislabeled as missing headset");checkHeadsetSystem(XR_SUCCESS);
    require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"COM failed");
    for(bool failure:{false,true}){
        completion=CreateEventW(nullptr,TRUE,FALSE,nullptr);require(completion!=nullptr,"event failed");
        calls.clear();failSession=failure;
        {
            IntroApp app;current=&app;
            app.xrDestroySwapchain=destroySwapchain;app.xrDestroySpace=destroySpace;
            app.xrDestroySession=destroySession;app.xrDestroyActionSet=destroyActions;app.xrDestroyInstance=destroyInstance;
            app.eyes[0].chain=reinterpret_cast<XrSwapchain>(1);app.space=reinterpret_cast<XrSpace>(2);
            app.session=reinterpret_cast<XrSession>(3);app.actions=reinterpret_cast<XrActionSet>(4);app.instance=reinterpret_cast<XrInstance>(5);
            require(DuplicateHandle(GetCurrentProcess(),completion,GetCurrentProcess(),&app.released,0,FALSE,DUPLICATE_SAME_ACCESS),"duplicate failed");
            D3D_FEATURE_LEVEL level;
            require(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&app.device,&level,&app.context)),"WARP unavailable");
            app.renderer.initialize(app.device.Get());app.preview.initialize(app.device.Get(),false);
            require(app.scene.makeText(),"font missing");app.renderer.prepare(app.context.Get(),app.scene,2.f);
            std::array<XrView,2> views{};for(auto& view:views)view.pose.orientation.w=1;
            app.preview.draw(app.context.Get(),app.renderer,app.scene,views,2.f,false);
            require(app.shutdown()==!failure,"failed XR destroy not propagated");
            require(!app.device&&!app.context,"graphics device survives shutdown");
            require(app.shutdown()==!failure,"shutdown is not idempotent");
        }
        require(calls==std::vector<int>({1,2,3,4,5}),"XR destruction order incorrect or duplicated");
        require(WaitForSingleObject(completion,0)==WAIT_TIMEOUT,"destructor released game prematurely");
        CloseHandle(completion);
    }
    CoUninitialize();std::cout<<"PASS: GPU drain, desktop cleanup, ordered XR teardown, delayed handoff and failure propagation\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
