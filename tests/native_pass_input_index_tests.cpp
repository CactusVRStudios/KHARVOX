#include "../src/native/NativePassInputIndex.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

using namespace kharvox::native;
using Inputs=std::map<uintptr_t,int>;
using Index=PassInputIndex<Inputs>;
struct Pass {uintptr_t framebuffer;Inputs images;};
using Targets=std::map<uintptr_t,PassTargetIdentity>;
static void require(bool ok){if(!ok)std::abort();}
static PassTargetIdentity target(const Targets& targets,uintptr_t fb){
 auto it=targets.find(fb);return it==targets.end()?PassTargetIdentity{{},0,0,0,fb}:it->second;
}
// Independent r149 oracle: compare live framebuffer metadata for each pair,
// then merge every matching source pass in recording order.
static Index::Group scan(const Targets& targets,const std::vector<Pass>& passes,uintptr_t fb){
 Index::Group result;
 for(const auto& pass:passes){
  bool same=pass.framebuffer==fb;
  if(!same){auto a=targets.find(pass.framebuffer),b=targets.find(fb);
   same=a!=targets.end()&&b!=targets.end()&&a->second.attachments==b->second.attachments&&a->second.width==b->second.width&&a->second.height==b->second.height&&a->second.layers==b->second.layers;}
  if(!same)continue;
  ++result.passes;
  for(auto [view,layout]:pass.images){auto [it,added]=result.images.emplace(view,layout);if(!added&&it->second!=layout)result.conflict=true;}
 }
 return result;
}
static void build(Index& index,const Targets& targets,const std::vector<Pass>& passes){
 index.clear();for(const auto& pass:passes)index.add(target(targets,pass.framebuffer),pass.images);
}
static void compare(const Index& index,const Targets& targets,const std::vector<Pass>& passes,uintptr_t fb){
 auto expected=scan(targets,passes,fb);auto actual=index.find(target(targets,fb));
 require(bool(actual)==bool(expected.passes));
 if(actual)require(actual->images==expected.images&&actual->conflict==expected.conflict&&actual->passes==expected.passes);
}
static void tests(){
 Targets targets{{1,{{10,20},1920,1080,1}},{2,{{10,20},1920,1080,1}},
 {3,{{20,10},1920,1080,1}},{4,{{10,20},1921,1080,1}},
 {5,{{10,20},1920,1081,1}},{6,{{10,20},1920,1080,2}},
 {7,{{},1920,1080,1}},{8,{{},1920,1080,1}}};
 std::vector<Pass> passes{{1,{{100,1}}},{2,{{101,2}}},{1,{{102,3}}},
 {3,{{100,4}}},{4,{}},{5,{}},{6,{}},{7,{{103,2}}},{90,{{104,1}}}};
 Index index;build(index,targets,passes);
 for(uintptr_t fb=1;fb<100;++fb)compare(index,targets,passes,fb);
 require(index.find(target(targets,2))->images.size()==3);
 // New captured inputs invalidate/rebuild; conflicting repeated layouts
 // remain errors for that target, without poisoning unrelated targets.
 passes.push_back({2,{{100,9}}});build(index,targets,passes);
 require(index.find(target(targets,1))->conflict);
 require(!index.find(target(targets,3))->conflict);
 // Next frame can reuse a framebuffer handle with different attachments.
 targets[1]={{99},640,480,1};passes={{1,{{555,8}}}};build(index,targets,passes);
 require(!index.find(target(targets,2)));
 compare(index,targets,passes,1);
 index.clear();require(index.size()==0&&index.sourcePasses==0&&index.sourceInputs==0);
 // Differential coverage of collapse, repeat, reordering, aliases, missing
 // metadata, layout conflicts and changing targets across 200 frames.
 std::mt19937 random(150);
 for(int frame=0;frame<200;++frame){
  targets.clear();passes.clear();
  for(uintptr_t fb=1;fb<=60;++fb){auto family=random()%12;
   if(fb%11)targets[fb]={{family*2+1,family*2+2},uint32_t(640+family%2),480,1};}
  for(int p=0;p<200;++p){Pass pass{random()%65+1,{}};
   for(int i=0;i<4;++i)pass.images[random()%20+100]=int(random()%4);
   passes.push_back(std::move(pass));}
  build(index,targets,passes);
  for(uintptr_t fb=1;fb<=70;++fb)compare(index,targets,passes,fb);
 }
 std::cout<<"Pass input index differential/lifetime tests passed\n";
}
static void benchmark(size_t count){
 Targets targets;std::vector<Pass> passes;std::vector<uintptr_t> queries;
 for(size_t p=0;p<count;++p){uintptr_t family=p%(count/2)+1;
  targets[p+1]={{family*2,family*2+1},1920,1080,1};
  targets[p+1+count]=targets[p+1]; // distinct handles, same attachments
  passes.push_back({p+1,{{family+1000,1},{family+2000,2}}});queries.push_back(p+1+count);}
 std::vector<double> oldTimes,newTimes;uint64_t checksum=0;
 for(int round=0;round<101;++round){
  const auto oldRun=[&]{auto start=std::chrono::steady_clock::now();for(auto fb:queries)checksum+=scan(targets,passes,fb).images.size();return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();};
  const auto newRun=[&]{auto start=std::chrono::steady_clock::now();Index index;build(index,targets,passes);for(auto fb:queries){auto group=index.find(target(targets,fb));if(group)checksum+=group->images.size();}return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();};
  // Alternate order; include index construction, identity copies and union.
  if(round%2){newTimes.push_back(newRun());oldTimes.push_back(oldRun());}
  else {oldTimes.push_back(oldRun());newTimes.push_back(newRun());}
 }
 std::sort(oldTimes.begin(),oldTimes.end());std::sort(newTimes.begin(),newTimes.end());
 std::cout<<"Synthetic CPU only: passes="<<count<<" queries="<<count<<" oldCandidates="<<count*count<<" indexedSourceVisits="<<count<<" oldMedianUs="<<oldTimes[50]<<" indexedMedianUs="<<newTimes[50]<<" checksum="<<checksum<<" (includes index build; no Vulkan/GPU/FPS claim)\n";
}
int main(int argc,char** argv){tests();if(argc>1&&std::string(argv[1])=="--benchmark"){benchmark(64);benchmark(256);}}
