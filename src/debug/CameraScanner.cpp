#include "../common/DiagnosticLogging.h"
#include "CameraScanner.h"
#include "../common/RuntimePaths.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace {
struct Candidate { float* address{}; std::array<float,9> baseline{}; float matrixScore{}; float change{}; };
std::vector<Candidate> candidates;
std::atomic<bool> started{};
Candidate best{};

void log(const std::string& text){
    if (!kharvox::extendedDiagnosticsEnabled()) return;char temp[MAX_PATH]{};GetTempPathA(MAX_PATH,temp);std::ofstream out(std::string(temp)+"KHARVOX.log",std::ios::app);out<<"[KHARVOX][CAMSCAN] "<<text<<'\n';}
bool readableWritable(DWORD p){p&=0xff;return p==PAGE_READWRITE||p==PAGE_WRITECOPY||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;}
float score(const float*m){for(int i=0;i<9;i++)if(!std::isfinite(m[i])||std::fabs(m[i])>1.1f)return 0;float err=0;for(int r=0;r<3;r++){float l=0;for(int c=0;c<3;c++)l+=m[r*3+c]*m[r*3+c];err+=std::fabs(l-1);}for(int a=0;a<3;a++)for(int b=a+1;b<3;b++){float d=0;for(int c=0;c<3;c++)d+=m[a*3+c]*m[b*3+c];err+=std::fabs(d);}return std::max(0.f,1.f-err/6.f);}
bool safeRead(float*address,std::array<float,9>&out){MEMORY_BASIC_INFORMATION mi{};if(!VirtualQuery(address,&mi,sizeof(mi))||mi.State!=MEM_COMMIT||!readableWritable(mi.Protect)||(mi.Protect&PAGE_GUARD))return false;auto end=reinterpret_cast<uintptr_t>(mi.BaseAddress)+mi.RegionSize;if(reinterpret_cast<uintptr_t>(address)+sizeof(out)>end)return false;std::memcpy(out.data(),address,sizeof(out));return true;}
void capture(){candidates.clear();SYSTEM_INFO si{};GetSystemInfo(&si);auto p=reinterpret_cast<unsigned char*>(si.lpMinimumApplicationAddress);auto max=reinterpret_cast<unsigned char*>(si.lpMaximumApplicationAddress);MEMORY_BASIC_INFORMATION mi{};size_t scanned=0;while(p<max&&VirtualQuery(p,&mi,sizeof(mi))){auto next=p+mi.RegionSize;bool scoped=mi.Type==MEM_IMAGE||(mi.Type==MEM_PRIVATE&&mi.RegionSize<=8ull*1024*1024);if(scoped&&mi.State==MEM_COMMIT&&readableWritable(mi.Protect)&&!(mi.Protect&(PAGE_GUARD|PAGE_NOCACHE))&&mi.RegionSize>=4096&&scanned<256ull*1024*1024){auto begin=reinterpret_cast<unsigned char*>(mi.BaseAddress);auto bytes=std::min<size_t>(mi.RegionSize,256ull*1024*1024-scanned);scanned+=bytes;for(size_t off=0;off+sizeof(float)*9<=bytes;off+=16){auto f=reinterpret_cast<float*>(begin+off);float s=score(f);if(s>0.992f){Candidate c;c.address=f;std::memcpy(c.baseline.data(),f,sizeof(c.baseline));c.matrixScore=s;candidates.push_back(c);if(candidates.size()>=50000)break;}}}if(candidates.size()>=50000||scanned>=256ull*1024*1024)break;if(next<=p)break;p=next;}log("F6 baseline captured candidates="+std::to_string(candidates.size())+" scannedMB="+std::to_string(scanned/1024/1024)+"; rotate DOOM camera, then press F7");}
void compare(){std::vector<Candidate> ranked;ranked.reserve(candidates.size());for(auto c:candidates){std::array<float,9> now{};if(!safeRead(c.address,now))continue;float currentScore=score(now.data());if(currentScore<0.98f)continue;float delta=0;for(int i=0;i<9;i++)delta+=std::fabs(now[i]-c.baseline[i]);if(delta<0.03f)continue;c.change=delta;c.matrixScore=currentScore;ranked.push_back(c);}std::sort(ranked.begin(),ranked.end(),[](auto&a,auto&b){return a.matrixScore+a.change*.05f>b.matrixScore+b.change*.05f;});if(!ranked.empty())best=ranked.front();log("F7 reactive orthonormal candidates="+std::to_string(ranked.size()));for(size_t i=0;i<std::min<size_t>(20,ranked.size());i++){auto&c=ranked[i];std::ostringstream o;o<<'#'<<(i+1)<<" addr="<<c.address<<" matrixScore="<<c.matrixScore<<" change="<<c.change;log(o.str());}if(!ranked.empty())log("F9 holds +15deg yaw on #1; F10 holds +10deg pitch on #1");}
void rotate(float*out,const float*base,float yaw,float pitch){float cy=std::cos(yaw),sy=std::sin(yaw),cp=std::cos(pitch),sp=std::sin(pitch);float r[9]={cy,sy*sp,sy*cp,0,cp,-sp,-sy,cy*sp,cy*cp};for(int a=0;a<3;a++)for(int b=0;b<3;b++){float v=0;for(int k=0;k<3;k++)v+=base[a*3+k]*r[k*3+b];out[a*3+b]=v;}}
DWORD WINAPI worker(void*){for(;;){SHORT k6=GetAsyncKeyState(VK_F6),k7=GetAsyncKeyState(VK_F7);const auto captureMarker=kharvox::runtimePath(L"camscan_capture"),compareMarker=kharvox::runtimePath(L"camscan_compare");bool file6=DeleteFileW(captureMarker.c_str())!=FALSE,file7=DeleteFileW(compareMarker.c_str())!=FALSE;if((k6&1)||file6)capture();if((k7&1)||file7)compare();if(best.address){float out[9];if(GetAsyncKeyState(VK_F9)&0x8000){rotate(out,best.baseline.data(),15.f*0.01745329252f,0);std::memcpy(best.address,out,sizeof(out));}else if(GetAsyncKeyState(VK_F10)&0x8000){rotate(out,best.baseline.data(),0,10.f*0.01745329252f);std::memcpy(best.address,out,sizeof(out));}}Sleep(2);}return 0;}
}

void KharvoxCameraScannerStart(){if(started.exchange(true))return;auto thread=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(thread){CloseHandle(thread);log("ready: F6 baseline, move mouse camera, F7 rank, hold F9/F10 test");}else log("worker creation failed");}
