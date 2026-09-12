#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <chrono>
#include "../src/intro/CubeD3D11.h"
#include "../src/intro/DesktopPreview.h"
using Microsoft::WRL::ComPtr;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char**argv){try{
    SetProcessDPIAware();require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"COM initialization failed");
    require(argc==2,"output directory required");std::filesystem::path output=argv[1];std::filesystem::create_directories(output);
    kharvox::intro::InputGate input;
    require(!input.update(true,true),"unpresented scene dismisses");input.presented=true;
    require(!input.update(false,true),"unfocused held input dismisses");
    require(input.update(true,true)&&input.dismissed,"held input must dismiss after presentation and focus");
    require(!input.update(true,true),"duplicate dismiss");
    kharvox::intro::InputGate fresh;fresh.presented=true;
    require(!fresh.update(true,false),"idle input dismisses");
    require(fresh.update(true,true),"fresh press does not dismiss");
    kharvox::intro::Scene scene;require(scene.makeText(),"text missing");
    kharvox::intro::Scene direct;require(direct.makeText(true),"direct demo text missing");
    require(direct.footerQuads.size()!=scene.footerQuads.size(),"direct demo still shows launch DOOM hint");
    for(size_t i=0;i<scene.textQuads.size();i++)require(direct.textQuads[i].size()==scene.textQuads[i].size(),"direct mode changes main text");
    std::array<XrView,2> eyes{};for(int e=0;e<2;e++){eyes[e].pose.orientation.w=1;eyes[e].pose.position.x=e?.032f:-.032f;eyes[e].fov={-.8f,.7f,.75f,-.7f};}scene.setAnchor(eyes);
    require(fabsf(scene.anchor.x)<.001f&&fabsf(scene.anchor.z)<.001f,"viewer not inside cube horizontally");
    require(fabsf(eyes[0].pose.position.y-scene.anchor.y)<2.5f,"viewer outside cube vertically");
    require(kharvox::intro::Scene::halfSize*2==5.f,"cube is not five metres");
    for(float t:{0.f,1.f,2.f,3.f,6.f,9.f,12.f,15.f,18.f,23.f}){
        int contacts=0,free=0;for(auto d:scene.points(t)){
            require(std::isfinite(d.x)&&std::isfinite(d.y)&&std::isfinite(d.z),"nonfinite morph");
            require(fabsf(d.x)<=2.5001f&&fabsf(d.y)<=2.5001f&&d.z>=-6.5001f&&d.z<=2.5001f,"particle escaped stationary cube");
            if(d.color==16){++contacts;require(fabsf(fabsf(d.x)-2.5f)<.001f||fabsf(fabsf(d.y)-2.5f)<.001f||fabsf(d.z+6.5f)<.001f||fabsf(d.z-2.5f)<.001f,"contact not on cube wall");}else ++free;
        }require(contacts>0&&free>0,"missing wall contact or moving shape");
    }
    require(scene.frontZ==-6.5f&&scene.backZ==2.5f,"front extension moved the rear wall or viewer");
    for(unsigned i=0;i<scene.blockCount;i++){
        require(scene.blockIndex(scene.blockStart(i)+.001f)==i&&scene.blockIndex(scene.blockStart(i)+scene.blockDuration(i)-.001f)==i,"staggered block timing");
        require(!scene.textBlocks[i].empty(),"missing text block");
        for(unsigned line=0;line<scene.blocks()[i].size();line++)if(!scene.blocks()[i][line].empty()){
            bool found=false;for(const auto& q:scene.textQuads[i])if(q.line==line&&q.mode==1)found=true;
            require(found,"text line clipped or missing from long block");
        }
        float minX=10,maxX=-10,minY=10,maxY=-10;
        for(auto d:scene.textBlocks[i]){require(d.z< -6.48f&&d.z> -6.5f,"text not attached to extended wall");minX=std::min(minX,d.x);maxX=std::max(maxX,d.x);minY=std::min(minY,d.y);maxY=std::max(maxY,d.y);}
        require(fabsf(minY+maxY)<.005f,"text not vertically centred on wall");
        require(fabsf(minX+maxX)<.005f&&maxX<2.3f,"text block not centred or exceeds wall");
    }
    require(scene.blockIndex(scene.blockStart(scene.blockCount)+.001f)==0&&scene.blockIndex(scene.blockStart(scene.blockCount)+scene.blockDuration(0)+.001f)==1,"text cycle does not repeat");
    require(!scene.footer.empty(),"permanent launch hint missing");
    require(scene.copperBand(0.f,0.f)!=scene.copperBand(0.f,.5f),"copper colors are not animated");
    require(scene.copperBand(0.f,0.f)!=scene.copperBand(.1f,0.f),"copper has no horizontal color bands");
    require(scene.copperBand(.24f,.25f)==scene.copperBand(0.f,0.f),"copper does not travel right");
    require(scene.lineBounce(0,-1)==0&&scene.lineBounce(0,.1f)>0&&scene.lineBounce(0,.4f)==0,"beat envelope invalid");
    require(scene.lineBounce(0,.1f)!=scene.lineBounce(2,.1f),"lines bounce identically");
    auto fixedHint=scene.project(eyes[0],1000,1000,0.f,.1f)[kharvox::intro::footerColor];
    for(float time:{0.f,7.f,14.f,21.f,28.f,35.f}){
        auto page=scene.project(eyes[0],1000,1000,time,.1f);
        require(!page[kharvox::intro::outlineColor].empty(),"fixed outline missing");
        require(!page[kharvox::intro::footerColor].empty(),"launch hint disappeared on a text block");
        const auto& hint=page[kharvox::intro::footerColor];require(hint.size()==fixedHint.size(),"launch hint changes between blocks");
        for(size_t i=0;i<hint.size();i++)require(hint[i].x==fixedHint[i].x&&hint[i].y==fixedHint[i].y&&hint[i].width==fixedHint[i].width&&hint[i].height==fixedHint[i].height,"launch hint is not static");
    }
    for(unsigned block=0;block<scene.blockCount;block++){
        require(fabsf(scene.blockDuration(block)-scene.revealEnd(block)-8.f)<.0001f,"full block is not held eight seconds");
        unsigned order=0;
        for(unsigned line=0;line<scene.blocks()[block].size();line++)if(!scene.blocks()[block][line].empty()){
            require(scene.lineProgress(block,line,order*scene.lineDelay)==0,"line appears too early");
            require(scene.lineProgress(block,line,order*scene.lineDelay+scene.revealSeconds+.001f)==1,"line does not finish revealing");
            ++order;
        }
        const auto settledTime=scene.blockStart(block)+scene.revealEnd(block)+.01f;
        auto settled=scene.geometry(settledTime);
        require(settled.size()==scene.textQuads[block].size()+1176+scene.footerQuads.size(),"settled text incomplete");
        for(size_t i=0;i<scene.textQuads[block].size();i++){
            const auto& original=scene.textQuads[block][i];const auto& shown=settled[i];
            require(original.x==shown.x&&original.y==shown.y&&original.width==shown.width&&original.height==shown.height,"text keeps moving after reveal");
        }
    }
    for(unsigned effect=0;effect<3;effect++){
        scene.effectSeed=0;while(scene.revealEffect(scene.page(0))!=effect)++scene.effectSeed;
        auto partial=scene.geometry(.12f);
        for(const auto& quad:partial)require(std::isfinite(quad.x)&&quad.width>0&&quad.height>0,"invalid reveal geometry");
        for(size_t i=0;i<partial.size()-1176-scene.footerQuads.size();i++)require(partial[i].line==0,"later line revealed too soon");
    }
    auto still=scene.geometry(21.f,-1.f),bounce=scene.geometry(21.f,.1f);
    require(still.size()==bounce.size(),"beat changed geometry count");
    const size_t footerStart=still.size()-scene.footerQuads.size();
    for(size_t i=0;i<still.size();i++){
        require(still[i].x==bounce[i].x,"beat moved text horizontally");
        if(i<footerStart)require(still[i].y==bounce[i].y,"beat moved main block or particles");
        else require(bounce[i].y>still[i].y&&bounce[i].y-still[i].y<.065f,"footer does not gently bounce");
    }
    for(const auto& quads:scene.textQuads)require(!quads.empty()&&quads.size()<10000,"text geometry not compact");
    const auto benchStart=std::chrono::steady_clock::now();size_t rectCount=0;
    for(int frame=0;frame<120;frame++){
        auto geometry=scene.geometry(21.f+frame/90.f,.1f);rectCount+=geometry.size();
    }
    std::cout<<"Geometry stereo ms/frame: "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-benchStart).count()/120
        <<"; rectangles/eye: "<<rectCount/120<<'\n';
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> base;ComPtr<ID3D11DeviceContext1> context;
    D3D_FEATURE_LEVEL level{};require(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&base)),"D3D11 WARP unavailable");require(SUCCEEDED(base.As(&context)),"D3D11.1 unavailable");
    D3D11_TEXTURE2D_DESC desc{};desc.Width=1000;desc.Height=1000;desc.MipLevels=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;require(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&texture)),"texture creation failed");ComPtr<ID3D11RenderTargetView> target;
    require(FAILED(device->CreateRenderTargetView(texture.Get(),nullptr,&target)),"typeless regression fixture must reject inferred view");
    require(SUCCEEDED(kharvox::intro::createEyeTarget(device.Get(),texture.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,&target)),"explicit typeless eye target creation failed");
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM}){
        ComPtr<ID3D11RenderTargetView> typed;
        require(SUCCEEDED(kharvox::intro::createEyeTarget(device.Get(),texture.Get(),format,&typed)),"negotiated eye format rejected");
        D3D11_RENDER_TARGET_VIEW_DESC actual{};typed->GetDesc(&actual);
        require(actual.Format==format,"negotiated view format changed");
    }
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;require(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)),"staging creation failed");
    kharvox::intro::CubeRenderer renderer;renderer.initialize(device.Get());
    kharvox::intro::DesktopPreview preview;preview.initialize(device.Get(),false);
    RECT client{};GetClientRect(preview.handle(),&client);
    require(client.right==1280&&client.bottom==720,"desktop client is not 720p");
    const auto desktop=kharvox::intro::desktopView(eyes);
    require(fabsf(tanf(desktop.fov.angleRight)/tanf(desktop.fov.angleUp)-1280.f/720.f)<.001f,"desktop aspect distorted");
    renderer.prepare(context.Get(),scene,0.f,.1f);
    preview.draw(context.Get(),renderer,scene,eyes,0.f,false);
    kharvox::intro::DesktopPreview::pump();
    std::array<std::vector<unsigned char>,2> stereo,motion;
    for(int shot=0;shot<13;shot++){
        int eye=shot==1?1:0;bool black=shot==7;float t=shot>=10?scene.blockStart(shot-4)+scene.revealEnd(shot-4)+.1f:shot>=8?21.f:shot==7?0.f:shot<2?1.5f:scene.blockStart(shot-1)+scene.revealEnd(shot-1)+.1f;
        renderer.prepare(context.Get(),scene,t,shot==8?-1.f:.1f);
        renderer.drawEye(context.Get(),target.Get(),scene,eyes[eye],1000,1000,t,black);
        context->CopyResource(staging.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE mapped{};require(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"GPU readback failed");
        std::vector<unsigned char> pixels;pixels.reserve(3000000);int colored=0;
        for(unsigned y=0;y<1000;y++)for(unsigned x=0;x<1000;x++){auto p=static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch+x*4;pixels.insert(pixels.end(),p,p+3);if(p[0]>100||p[1]>100)++colored;if(black)require(p[0]==0&&p[1]==0&&p[2]==0,"handoff image is not black");}
        context->Unmap(staging.Get(),0);if(!black)require(colored>1000,"scene not rendered by D3D11");
        std::ofstream image(output/("frame-"+std::to_string(shot)+".ppm"),std::ios::binary);image<<"P6\n1000 1000\n255\n";image.write(reinterpret_cast<char*>(pixels.data()),pixels.size());
        if(shot<2)stereo[shot]=pixels;
        if(shot>=8&&shot<10)motion[shot-8]=pixels;
    }
    require(std::equal(motion[0].begin(),motion[0].begin()+550*3000,motion[1].begin()),"GPU beat moved main block or particles");
    require(motion[0]!=motion[1],"GPU footer did not bounce");
    require(stereo[0]!=stereo[1],"stereo parallax missing");
    unsigned surroundingParticles=0;
    for(float yaw:{0.f,1.5707963f,3.1415927f,4.712389f}){
        auto look=eyes[0];look.pose.orientation={0,sinf(yaw*.5f),0,cosf(yaw*.5f)};
        auto around=scene.project(look,1000,1000,1.25f);
        for(unsigned c=0;c<17;c++)surroundingParticles+=unsigned(around[c].size());
        if(yaw>3.f&&yaw<3.2f)for(unsigned c=kharvox::intro::copperBase;c<around.size();c++)require(around[c].empty(),"wall text follows viewer when looking behind");
    }
    require(surroundingParticles>100,"particle room not visible when looking around");
    auto floorScene=scene;floorScene.setAnchor(eyes,-1.7f);
    require(fabsf(floorScene.anchor.y-2.5f+1.7f)<.001f,"cube bottom does not match tracked floor");
    auto anchor=scene.anchor;eyes[0].pose.position.x+=.4f;auto moved=scene.project(eyes[0],1000,1000,1.25f);require(scene.anchor.x==anchor.x,"scene follows headset");
    for(auto& batch:moved)for(auto r:batch)require(r.x>=0&&r.y>=0&&r.x+r.width<=1000&&r.y+r.height<=1000,"projection outside eye");
    std::cout<<"PASS: wall contacts, rear-wall text, stereo D3D11 rendering, black handoff, input gate\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
