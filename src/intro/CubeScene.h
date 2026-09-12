#pragma once
#include <windows.h>
#include <openxr/openxr.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include "IntroAssets.h"

namespace kharvox::intro {
struct Dot { float x,y,z,size; unsigned color; unsigned line{}; };
struct Quad { float x,y,z,width,height; std::array<float,4> color; float mode; unsigned line{}; };
struct Rect { int x,y,width,height; };
inline constexpr unsigned copperBase=17,copperCount=32,footerColor=copperBase+copperCount,outlineColor=footerColor+1;
using Batches=std::array<std::vector<Rect>,outlineColor+1>;
inline XrVector3f rotate(XrQuaternionf q,XrVector3f v){
    XrVector3f t{2*(q.y*v.z-q.z*v.y),2*(q.z*v.x-q.x*v.z),2*(q.x*v.y-q.y*v.x)};
    return {v.x+q.w*t.x+q.y*t.z-q.z*t.y,v.y+q.w*t.y+q.z*t.x-q.x*t.z,v.z+q.w*t.z+q.x*t.y-q.y*t.x};
}
struct InputGate {
    bool presented{}, dismissed{};
    bool update(bool focused,bool down){
        if(!focused||!presented||dismissed)return false;
        // A held grip must not require releasing every bound input first.
        if(down){dismissed=true;return true;}
        return false;
    }
};
struct Scene {
    static constexpr float halfSize=2.5f;
    static constexpr float frontZ=-6.5f, backZ=2.5f;
    static constexpr float textZ=frontZ+.015f;
    static constexpr float lineDelay=.60f,revealSeconds=.70f,holdSeconds=8.f;
    static constexpr unsigned blockCount=9;
    static const std::array<std::vector<std::wstring>,blockCount>& blocks(){
        static const std::array<std::vector<std::wstring>,blockCount> value{{
            {L"Welcome to KHARVOX",L"A VR MOD FOR DOOM 2016",L"",L"MADE BY CACTUS VR"},
            {L"VR IS DEAD!"},
            {L"FUCK NO!",L"WE'RE BACK!",L"",L"ANOTHER GAME.",L"ANOTHER VR MOD"},
            {L"THE VR SCENE IS ON FIRE!",L"",L"",L"CAN WE AGREE THAT THIS IS COOL?",L"",L"300 KB OF RAW VR CRACKTRO OLD-SCHOOL FUN",L"",L"THIS SHOULD BE THE NEW STANDARD",L"FOR VR MODS FROM NOW ON"},
            {L"NOT YOUR AVERAGE AI SLOP MOD",L"",L"Code: Cactus VR",L"VR Hands: Dishlan",L"Debugging: Nabelo, Vince Crusty",L"Native Renderer: TinyBlackDog",L"Intro inspired by: SoLO",L"Music: COMA"},
            {L"SPECIAL THANKS TO OUR TESTERS",L"",L"NABELO, VINCE CRUSTY",L"TINO, HOSHI82, VR DAD"},
            {L"SHOUTOUTS TO",L"",L"Flat2VR Discord, Gamertag VR",L"Beardo Benjo, 8Hitman2, BMFVR",L"emvierdeh, id Software, VOODOO VR",L"John Carmack, John Romero, Nathie",L"Eric Provencher, Mo Fun VR",L"Cactus Cowboy Discord - you guys rock!",L"Flat2VR Studios, Galaghan, StockiVR",L"Michel, PuRe, Shane, sbsce",L"Hellcat, LGZakx, Gaming Lady Nici",L"Treiber, Data, Miku, MantaXmobile"},
            {L"GREETINGS TO FELLOW MODDERS",L"",L"Cabalistic, Praydog, elliotttate",L"PureDark, GeT-RiCh, Rusty Gere",L"GingasVR, Crementif, Ashok",L"VRified Games, LunchAndVR, GanJJ_",L"Dr. Beef, BaggyG, Bummser - TEAM BEEF",L"Takemann, fewerwrong, TinyBlackDog",L"thefreemike, NotGodlike, GameOrDie",L"Mr-N1ce, MrSurviv0r"},
            {L"NOW GO AND PLAY DOOM IN VR!"}
        }};return value;
    }
    static unsigned lineCount(unsigned block){unsigned count=0;for(const auto& line:blocks()[block])if(!line.empty())++count;return count;}
    static float revealEnd(unsigned block){return (lineCount(block)-1)*lineDelay+revealSeconds;}
    static float blockDuration(unsigned block){return revealEnd(block)+holdSeconds;}
    static float blockStart(unsigned block){float start=0;for(unsigned i=0;i<block;i++)start+=blockDuration(i);return start;}
    struct Page { unsigned block; float age; unsigned cycle; };
    static Page page(float time){
        const float duration=blockStart(blockCount);const float positive=std::max(0.f,time);
        float age=std::fmod(positive,duration);unsigned block=0;
        while(block+1<blockCount&&age>=blockDuration(block)){age-=blockDuration(block);++block;}
        return {block,age,unsigned(positive/duration)};
    }
    static unsigned blockIndex(float time){return page(time).block;}
    static float lineProgress(unsigned block,unsigned line,float age){
        unsigned order=0;for(unsigned i=0;i<line;i++)if(!blocks()[block][i].empty())++order;
        return std::clamp((age-order*lineDelay)/revealSeconds,0.f,1.f);
    }
    uint32_t effectSeed=uint32_t(GetTickCount64())^GetCurrentProcessId();
    unsigned revealEffect(Page current)const{
        uint32_t hash=effectSeed+(current.cycle*blockCount+current.block)*0x9e3779b9u;
        hash^=hash>>16;hash*=0x7feb352du;hash^=hash>>15;
        return hash%3;
    }
    static unsigned copperBand(float x,float time){
        const float wave=x*1.25f-time*1.2f;
        return copperBase+unsigned((wave-std::floor(wave))*copperCount)%copperCount;
    }
    XrVector3f anchor{};float yaw{};bool anchored{};
    float eyeLocalY=-.85f;
    std::array<std::vector<Dot>,blockCount> textBlocks;
    std::vector<Dot> footer;
    std::array<std::vector<Quad>,blockCount> textQuads;
    std::vector<Quad> footerQuads;
    std::array<std::vector<float>,blockCount> textLineY;
    void setAnchor(const std::array<XrView,2>& eyes,float floorY=NAN){
        anchor={(eyes[0].pose.position.x+eyes[1].pose.position.x)*.5f,(eyes[0].pose.position.y+eyes[1].pose.position.y)*.5f,(eyes[0].pose.position.z+eyes[1].pose.position.z)*.5f};
        const float headY=anchor.y;
        anchor.y=(std::isfinite(floorY)?floorY:headY-1.65f)+halfSize;
        const float nextEyeY=headY-anchor.y;
        for(auto& dot:footer)dot.y+=nextEyeY-eyeLocalY;
        for(auto& quad:footerQuads)quad.y+=nextEyeY-eyeLocalY;
        eyeLocalY=nextEyeY;
        auto f=rotate(eyes[0].pose.orientation,{0,0,-1});yaw=atan2f(-f.x,-f.z);anchored=true;
    }
    bool makeText(bool standalone=false){
        PrivateFont privateFont;if(!privateFont.handle)return false;
        HDC dc=CreateCompatibleDC(nullptr);if(!dc)return false;
        constexpr int width=1024,height=768;
        constexpr float pixel=.0044f;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        void* pixels{};auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!bitmap){DeleteDC(dc);return false;}
        auto old=SelectObject(dc,bitmap);
        SetTextColor(dc,RGB(255,255,255));SetBkColor(dc,0);SetTextAlign(dc,TA_CENTER);
        auto raster=[&](const std::vector<std::wstring>& lines,std::vector<Dot>& output,float centerY,int wantedHeight,unsigned color,std::vector<Quad>& quads){
            HFONT font{};HGDIOBJ oldFont{};int fontHeight=wantedHeight;
            for(;fontHeight>=12;--fontHeight){
                font=CreateFontW(fontHeight,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,NONANTIALIASED_QUALITY,DEFAULT_PITCH,L"04b");
                if(!font)return false;
                oldFont=SelectObject(dc,font);int widest=0;
                for(const auto& line:lines){SIZE size{};GetTextExtentPoint32W(dc,line.c_str(),int(line.size()),&size);widest=std::max(widest,int(size.cx));}
                if(widest<=width-48&&int(lines.size())*(fontHeight+12)<=height-16)break;
                SelectObject(dc,oldFont);DeleteObject(font);font=nullptr;
            }
            if(!font)return false;
            wchar_t face[LF_FACESIZE]{};GetTextFaceW(dc,LF_FACESIZE,face);
            bool correct=_wcsicmp(face,L"04b")==0;
            PatBlt(dc,0,0,width,height,BLACKNESS);
            const int lineHeight=fontHeight+12;
            const int top=(height-int(lines.size())*lineHeight)/2;
            for(size_t line=0;line<lines.size();line++)
                TextOutW(dc,width/2,top+int(line)*lineHeight,lines[line].c_str(),int(lines[line].size()));
            GdiFlush();output.clear();
            int minY=height,maxY=0,minX=width,maxX=0;
            for(int y=0;y<height;y++)for(int x=0;x<width;x++)if((static_cast<unsigned*>(pixels)[y*width+x]&0xffffff)!=0){
                minY=std::min(minY,y);maxY=std::max(maxY,y);minX=std::min(minX,x);maxX=std::max(maxX,x);
            }
            const float midY=(minY+maxY)*.5f,midX=(minX+maxX)*.5f;
            for(int y=minY;y<=maxY;y++)for(int x=minX;x<=maxX;x++)if((static_cast<unsigned*>(pixels)[y*width+x]&0xffffff)!=0)
                output.push_back({(x-midX)*pixel,centerY+(midY-y)*pixel,textZ,pixel*1.08f,color,unsigned(std::max(0,(y-top)/lineHeight))});
            // Build disjoint rectangles once, rather than projecting every font
            // pixel (and nine overlapping outline pixels) on every VR frame.
            std::vector<unsigned char> ink(width*height),outline(width*height);
            for(int y=0;y<height;y++)for(int x=0;x<width;x++)
                ink[y*width+x]=(static_cast<unsigned*>(pixels)[y*width+x]&0xffffff)!=0;
            if(color==copperBase)for(int y=2;y<height-2;y++)for(int x=2;x<width-2;x++)if(ink[y*width+x])
                for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++)outline[(y+dy)*width+x+dx]=1;
            quads.clear();
            auto pack=[&](const std::vector<unsigned char>& mask,unsigned tint,float mode){
                std::vector<Rect> packed;std::map<std::pair<int,int>,size_t> previous;
                for(int y=0;y<height;y++){
                    std::map<std::pair<int,int>,size_t> current;
                    for(int x=0;x<width;){
                        if(!mask[y*width+x]){++x;continue;}
                        int start=x;while(x<width&&mask[y*width+x])++x;
                        auto key=std::make_pair(start,x-start);auto found=previous.find(key);
                        size_t index;
                        if(found!=previous.end()){index=found->second;++packed[index].height;}
                        else {index=packed.size();packed.push_back({start,y,x-start,1});}
                        current[key]=index;
                    }
                    previous=std::move(current);
                }
                for(auto r:packed)quads.push_back({(r.x+(r.width-1)*.5f-midX)*pixel,
                    centerY+(midY-r.y-(r.height-1)*.5f)*pixel,textZ,r.width*pixel,r.height*pixel,Scene::color(tint),mode,unsigned(std::clamp(int((r.y+(r.height-1)*.5f-top)/lineHeight),0,int(lines.size())-1))});
            };
            if(color==copperBase)pack(outline,outlineColor,0);
            pack(ink,color,color==copperBase?1.f:0.f);
            SelectObject(dc,oldFont);DeleteObject(font);return correct&&!output.empty();
        };
        bool ok=true;for(size_t i=0;i<blocks().size();i++)ok=raster(blocks()[i],textBlocks[i],0.f,52,copperBase,textQuads[i])&&ok;
        for(unsigned block=0;block<blockCount;block++){
            textLineY[block].resize(blocks()[block].size());
            for(unsigned line=0;line<blocks()[block].size();line++){
                float low=10,high=-10;
                for(const auto& quad:textQuads[block])if(quad.line==line){low=std::min(low,quad.y-quad.height*.5f);high=std::max(high,quad.y+quad.height*.5f);}
                textLineY[block][line]=(low+high)*.5f;
            }
        }
        ok=raster({standalone?L"PRESS ANY BUTTON TO EXIT":L"PRESS ANY BUTTON TO LAUNCH DOOM IN VR"},footer,eyeLocalY-1.f,36,footerColor,footerQuads)&&ok;
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);return ok;
    }
    std::vector<Dot> points(float t)const{
        std::vector<Dot> dots;dots.reserve(1176);
        // The reference translates and rotates a particle object, then clamps
        // its vertices against stationary walls. Contact points turn grey.
        // Six walls make that same effect coherent when walking around in VR.
        const float cycle=fmodf(t/8.f,3.f);int shape=int(cycle);
        float mix=std::clamp((cycle-shape-.7f)/.3f,0.f,1.f);mix=mix*mix*(3-2*mix);
        const float tx=cosf(t*2.1f)*halfSize,ty=sinf(t*3.3f)*halfSize,tz=(frontZ+backZ)*.5f+sinf(t*1.5f)*(backZ-frontZ)*.5f;
        for(int face=0;face<6;face++)for(int v=0;v<14;v++)for(int u=0;u<14;u++){
            float a=2.f*u/13-1,b=2.f*v/13-1;XrVector3f cube;
            if(face<2)cube={face?1.f:-1.f,a,b};else if(face<4)cube={a,face==3?1.f:-1.f,b};else cube={a,b,face==5?1.f:-1.f};
            float len=sqrtf(cube.x*cube.x+cube.y*cube.y+cube.z*cube.z);
            float theta=6.2831853f*(u+face*14)/84,phi=6.2831853f*v/14;
            XrVector3f forms[3]={{cube.x/len*1.05f,cube.y/len*1.05f,cube.z/len*1.05f},{(.9f+.36f*cosf(phi))*cosf(theta),.36f*sinf(phi),(.9f+.36f*cosf(phi))*sinf(theta)},{cube.x*.9f,cube.y*.9f,cube.z*.9f}};
            auto p=forms[shape],q=forms[(shape+1)%3];p={p.x+(q.x-p.x)*mix,p.y+(q.y-p.y)*mix,p.z+(q.z-p.z)*mix};
            float cx=cosf(t*1.2f),sx=sinf(t*1.2f),cy=cosf(t*2.4f),sy=sinf(t*2.4f);
            float y=p.y*cx-p.z*sx,z=p.y*sx+p.z*cx;
            p={(p.x*cy+z*sy)*1.6f+tx,y*1.6f+ty,(-p.x*sy+z*cy)*1.6f+tz};
            bool hit=fabsf(p.x)>halfSize||fabsf(p.y)>halfSize||p.z<frontZ||p.z>backZ;
            unsigned color=(u*11+v*7+face*13+int(t*30))%16;
            dots.push_back({std::clamp(p.x,-halfSize,halfSize),std::clamp(p.y,-halfSize,halfSize),std::clamp(p.z,frontZ,backZ),hit?.025f:.032f,hit?16:color});
        }
        return dots;
    }
    static float lineBounce(unsigned line,float beatAge){
        const float age=beatAge-float(line)*.012f;
        if(age<=0||age>=.3f)return 0;
        return .065f*std::sin(age/.3f*3.1415927f)*std::exp(-age*3.f);
    }
    Batches project(const XrView& eye,int w,int h,float t,float beatAge=-1.f)const{
        Batches batches;
        auto q=eye.pose.orientation;q.x=-q.x;q.y=-q.y;q.z=-q.z;
        const float l=tanf(eye.fov.angleLeft),r=tanf(eye.fov.angleRight),up=tanf(eye.fov.angleUp),down=tanf(eye.fov.angleDown);
        auto add=[&](Dot d){
            XrVector3f world{anchor.x+cosf(yaw)*d.x+sinf(yaw)*d.z,anchor.y+d.y,anchor.z-sinf(yaw)*d.x+cosf(yaw)*d.z};
            auto p=rotate(q,{world.x-eye.pose.position.x,world.y-eye.pose.position.y,world.z-eye.pose.position.z});if(p.z>=-.05f)return;
            float depth=-p.z,px=(p.x/depth-l)/(r-l)*w,py=(up-p.y/depth)/(up-down)*h;
            int size=std::clamp(int(d.size/depth*w/(r-l)),1,96);
            if(!std::isfinite(px)||!std::isfinite(py)||px < -size||px>w+size||py < -size||py>h+size)return;
            int x0=std::clamp(int(px)-size/2,0,w),y0=std::clamp(int(py)-size/2,0,h),x1=std::clamp(int(px)+size/2+1,0,w),y1=std::clamp(int(py)+size/2+1,0,h);
            if(x1>x0&&y1>y0)batches[d.color].push_back({x0,y0,x1-x0,y1-y0});
        };
        for(auto d:textBlocks[blockIndex(t)]){

            auto outline=d;outline.color=outlineColor;
            // Expand in wall space, so the fixed contour follows the glyph in VR.
            for(float dx:{-.0088f,0.f,.0088f})for(float dy:{-.0088f,0.f,.0088f}){
                outline.x=d.x+dx;outline.y=d.y+dy;add(outline);
            }
            d.color=copperBand(d.x,t);add(d);
        }
        for(auto d:footer){d.y+=lineBounce(0,beatAge);add(d);}
        for(auto d:points(t))add(d);return batches;
    }
    std::vector<Quad> geometry(float time,float beatAge=-1.f)const{
        const auto current=page(time);const auto effect=revealEffect(current);
        std::vector<Quad> quads;quads.reserve(textQuads[current.block].size()+1176+footerQuads.size());
        for(auto quad:textQuads[current.block]){
            const float progress=lineProgress(current.block,quad.line,current.age);
            if(progress<=0)continue;
            if(progress<1){
                const float ease=1-std::pow(1-progress,3.f);
                if(effect<2){
                    // Fast scanner wipe, or a centre-out shutter. Fill and
                    // fixed contour share exactly the same clipping boundary.
                    const float left=effect==0?-2.3f:-2.3f*ease;
                    const float right=effect==0?-2.3f+4.6f*ease:2.3f*ease;
                    const float x0=std::max(quad.x-quad.width*.5f,left),x1=std::min(quad.x+quad.width*.5f,right);
                    if(x1<=x0)continue;quad.x=(x0+x1)*.5f;quad.width=x1-x0;
                }else{
                    // Brief CRT scanline unfold, with a small sideways snap.
                    const float centre=textLineY[current.block][quad.line],scale=.15f+.85f*ease;
                    quad.y=centre+(quad.y-centre)*scale;quad.height*=scale;
                    quad.x+=(quad.line%2?-.10f:.10f)*(1-ease);
                }
            }
            quads.push_back(quad);
        }
        for(auto d:points(time))quads.push_back({d.x,d.y,d.z,d.size,d.size,color(d.color),2});
        for(auto quad:footerQuads){quad.y+=lineBounce(0,beatAge);quads.push_back(quad);}
        return quads;
    }
    static std::array<float,4> color(unsigned c){
        if(c==footerColor)return {.85f,.85f,.92f,1};
        if(c==outlineColor)return {.32f,.34f,.48f,1};
        if(c>=copperBase){
            constexpr float neon[8][3]={{1,0,.55f},{.65f,0,1},{.08f,.2f,1},{0,1,1},
                {0,1,.35f},{.7f,1,0},{1,.85f,0},{1,.08f,.3f}};
            const float phase=float(c-copperBase)*8/copperCount;
            const int index=int(phase);const float mix=phase-index;
            std::array<float,4> result{0,0,0,1};
            for(int k=0;k<3;k++)result[k]=std::round((neon[index][k]*(1-mix)+neon[(index+1)%8][k]*mix)*31)/31;
            return result;
        }
        if(c==16)return {.55f,.55f,.55f,1};
        std::array<float,4> out{0,0,0,1};for(int k=0;k<3;k++)out[k]=.03f+.97f*std::max(0.f,cosf(c*6.2831853f/16-k*2.094395f));return out;
    }
};
}
