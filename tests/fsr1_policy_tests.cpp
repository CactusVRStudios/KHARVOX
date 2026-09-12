#define NOMINMAX
#include "../src/fsr/Fsr1Policy.h"
#include <cassert>
#include "../src/openxr/ProjectionSourceCrop.h"
#include "../src/native/NativePresentationPolicy.h"
#include "../src/openxr/OpenXRRuntimePolicy.h"
#include <cmath>
#include <initializer_list>

int main() {
    using namespace kharvox;
    struct TestFov{float angleLeft,angleRight,angleUp,angleDown;};
    constexpr float rad=3.14159265358979323846f/180.f;
    const TestFov rendered{-54*rad,54*rad,55*rad,-55*rad};
    const TestFov metaEyes[]={{-54*rad,40*rad,44*rad,-55*rad},{-40*rad,54*rad,44*rad,-55*rad}};
    for(const auto& eye:metaEyes){
        const auto crop=projectionSourceCrop(rendered,eye);
        for(float t:{0.f,.25f,.5f,.75f,1.f}){
            const float sourceU=crop[0]+t*(crop[1]-crop[0]);
            const float sourceRay=std::tan(rendered.angleLeft)+sourceU*(std::tan(rendered.angleRight)-std::tan(rendered.angleLeft));
            const float submittedRay=std::tan(eye.angleLeft)+t*(std::tan(eye.angleRight)-std::tan(eye.angleLeft));
            assert(std::fabs(sourceRay-submittedRay)<1.e-5f);
            const float sourceV=crop[2]+t*(crop[3]-crop[2]);
            const float sourceY=std::tan(rendered.angleUp)-sourceV*(std::tan(rendered.angleUp)-std::tan(rendered.angleDown));
            const float submittedY=std::tan(eye.angleUp)-t*(std::tan(eye.angleUp)-std::tan(eye.angleDown));
            assert(std::fabs(sourceY-submittedY)<1.e-5f);
        }
    }
    const auto leftCrop=projectionSourceCrop(rendered,metaEyes[0]);
    const auto rightCrop=projectionSourceCrop(rendered,metaEyes[1]);
    assert(leftCrop[0]==0.f&&rightCrop[1]==1.f&&leftCrop[1]<.9f&&rightCrop[0]>.1f);
    const auto unchanged=projectionSourceCrop(rendered,rendered);
    assert(unchanged[0]==0.f&&unchanged[1]==1.f&&unchanged[2]==0.f&&unchanged[3]==1.f);

    for (bool steam : {false,true}) for (bool native : {false,true}) {
        for (float scale : {.5f, .75f, 1.f, 1.1f, 1.2f, 1.5f, 2.f, 3.f, 5.f, 10.f}) {
            assert(sourceCarrierScale(scale,steam,native)==scale);
            assert(effectiveRenderScale(scale,steam)==scale);
            assert(openXrEyeTargetScale(scale,false,steam,native)==scale);
            assert(openXrEyeTargetScale(scale,true,steam,native)==1.f);
            assert(steamNativeResolutionScale(scale,steam)==1.f);
        }
    }
    uint32_t dimension=17;
    assert(scaledRenderDimension(3840,3.,16384,dimension) && dimension==11520);
    assert(scaledRenderDimension(2160,3.,16384,dimension) && dimension==6480);
    assert(scaledRenderDimension(4096,4.,16384,dimension) && dimension==16384);
    assert(!scaledRenderDimension(4096,4.01,16384,dimension) && dimension==16384);
    assert(!scaledRenderDimension(3840,1.e30,UINT32_MAX,dimension));
    assert(!scaledRenderDimension(3840,std::numeric_limits<double>::infinity(),UINT32_MAX,dimension));
    assert(!scaledRenderDimension(3840,std::numeric_limits<double>::quiet_NaN(),UINT32_MAX,dimension));
    assert(!scaledRenderDimension(3840,-1.,UINT32_MAX,dimension));
    assert(!scaledRenderDimension(3840,0.,UINT32_MAX,dimension));
    assert(scaledRenderDimension(1,2.5,100,dimension) && dimension==3);

    assert(requestFsr1Upscaling(true, true, 0.8f));
    assert(!requestFsr1Upscaling(false, true, 0.8f));
    assert(!requestFsr1Upscaling(true, false, 0.8f));
    assert(!requestFsr1Upscaling(true, true, 1.0f));
    assert(!requestFsr1Upscaling(true, true, 1.1f));
    assert(!requestFsr1Upscaling(true, true, 0.49f));

    assert(useFsr1ForFrame(true, true, false, false));
    assert(useFsr1ForFrame(true, true, true, false, true));
    assert(!useFsr1ForFrame(true, true, true, true, true));
    assert(!useFsr1ForFrame(true, false, true, false, false));
    // Gameplay -> cutscene -> first mono frame -> current new pair.
    assert(native::useCurrentPair(true,false,false));
    assert(!native::useCurrentPair(true,true,true));
    assert(!native::useCurrentPair(true,false,true)); // Old gameplay pair cannot cross cinematic entry.
    assert(native::useCurrentPair(true,false,true,true));
    assert(!native::useCurrentPair(true,false,false,true)); // Cinematic pair cannot cross exit.
    assert(native::renderScene(false,false,true));
    assert(native::renderScene(false,true,false));
    assert(!native::renderScene(true,true,true));
    assert(!native::renderScene(false,false,false));
    // Shared presentation decision + Native pair selection across a cinematic.
    for (bool immersive : {false, true}) {
        const bool cinematic = shouldKeepCinematicImmersive(
            immersive,false,true,true,false,false,false,false);
        const bool quad = selectQuadPresentation(
            false,false,false,true,cinematic,true,false,false,false,false,false);
        assert(quad == !immersive);
        assert(!native::useCurrentPair(true,quad,true));
        assert(native::useCurrentPair(true,quad,true,true)==immersive);
        assert(!native::useCurrentPair(false,false,false));
        assert(native::useCurrentPair(true,false,false));
    }
    // Selective Cine Window keeps ordinary cinematics in Quad, but allows assists.
    assert(!shouldKeepCinematicImmersive(true,true,true,true,false,false,false,false));
    assert(shouldKeepCinematicImmersive(true,true,true,true,false,false,true,false));
    assert(!native::useCurrentPair(false,false,false));
    assert(native::useCurrentPair(true,false,false));
    assert(!useFsr1ForFrame(true, false, false, false));
    assert(!useFsr1ForFrame(true, true, true, false));
    assert(!useFsr1ForFrame(true, true, false, true));

    for (bool directVirtualDesktop : {false, true})
        assert(std::fabs(openXrEyeTargetScale(0.7f, true,
            directVirtualDesktop) - 1.0f) < 0.0001f);
    assert(std::fabs(openXrEyeTargetScale(0.7f, false, true) - 0.7f) < 0.0001f);
    assert(std::fabs(openXrEyeTargetScale(1.4f, false, false) - 1.4f) < 0.0001f);

    assert(startupStablePresents(true, false, true) == 0u);
    assert(startupStablePresents(true, false, false) == 0u);
    assert(startupStablePresents(false, true, true) == 3u);
    assert(startupStablePresents(false, true, false) == 3u);
    assert(startupStablePresents(false, false, true) == 2u);
    assert(startupStablePresents(false, false, false) == 0u);
    assert(!inheritStartupPresentProgress(true));
    assert(inheritStartupPresentProgress(false));
    assert(deferSharedDeviceSessionCreation(true, false, 0u));
    assert(deferSharedDeviceSessionCreation(true, false, 2u));
    assert(!deferSharedDeviceSessionCreation(true, false, 3u));
    assert(!deferSharedDeviceSessionCreation(true, true, 0u));
    assert(!deferSharedDeviceSessionCreation(false, false, 0u));
    // Meta r228 trace: a one-Present startup swapchain is replaced before
    // another two Presents. Retired progress must not open the session gate.
    assert(!inheritStartupPresentProgress(false,true));
    uint32_t metaPresents=1;
    metaPresents=inheritStartupPresentProgress(false,true)?metaPresents:0;
    assert(metaPresents==0);
    for(;metaPresents<3;++metaPresents)
        assert(deferSharedDeviceSessionCreation(false,false,metaPresents,true));
    assert(!deferSharedDeviceSessionCreation(false,false,metaPresents,true));
    assert(!deferSharedDeviceSessionCreation(false,true,0,true));
    // Other runtimes retain immediate session eligibility.
    assert(inheritStartupPresentProgress(false,false));


    assert(quiesceFsrStartupSwapchain(true, true, 1u, 0u));
    assert(quiesceFsrStartupSwapchain(true, true, 2u, 0u));
    assert(quiesceFsrStartupSwapchain(true, true, 1u, 1u));
    assert(quiesceFsrStartupSwapchain(true, true, 3u, 8u));
    assert(!quiesceFsrStartupSwapchain(false, true, 1u, 0u));
    assert(!quiesceFsrStartupSwapchain(true, false, 1u, 0u));
    assert(!quiesceFsrStartupSwapchain(true, true, 0u, 0u));
    assert(!quiesceFsrStartupSwapchain(true, true, 1u, 9u));

    assert(safeHighResolutionCarrierScale(1.0f) == 1.0f);
    assert(safeHighResolutionCarrierScale(0.7f) == 0.7f);
    assert(safeHighResolutionCarrierScale(1.5f) == 1.5f);

    assert(useSteamFsrQuadStartupHandshake(true, true, true, false));
    assert(!useSteamFsrQuadStartupHandshake(true, true, true, true));
    assert(!useSteamFsrQuadStartupHandshake(true, true, false, false));
    assert(!useSteamFsrQuadStartupHandshake(true, false, true, false));
    assert(!useSteamFsrQuadStartupHandshake(false, true, true, false));
    return 0;
}
