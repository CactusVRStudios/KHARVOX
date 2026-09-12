#include "../src/native/NativeStereoPolicy.h"
#include "../src/native/NativeFrameSource.h"
#include "../src/native/NativeReplayRecording.h"
#include "../src/native/NativeCpuSamplingPolicy.h"
#include <functional>
#include <map>
#include <vector>
#include <iostream>
using namespace kharvox::native;
int main(){int errors=0;auto check=[&](bool b){if(!b)++errors;};
 FrameSourceIdentity source{41,7,SceneDomain::Gameplay};
 check(sourcePoseStable(source,41,41,7));
 check(!sourcePoseStable(source,40,41,7));check(!sourcePoseStable(source,41,42,7));
 check(!sourcePoseStable(source,41,41,8));check(!sourcePoseStable({},0,0,0));
 auto desired=source;desired.poseId=42;
 check(sameSceneContext(source,desired,false,false)); // newer XR prediction is normal
 check(!sameSceneContext(source,desired,false,true));
 desired.level=8;check(!sameSceneContext(source,desired,false,false));desired.level=7;
 for(auto domain:{SceneDomain::Gameplay,SceneDomain::Cinematic,SceneDomain::Scripted}){
   source.domain=domain;desired.domain=domain;check(sameSceneContext(source,desired,false,false));
   desired.domain=SceneDomain::Inactive;check(!sameSceneContext(source,desired,false,false));
 }
 source.domain=SceneDomain::Cinematic;desired.domain=SceneDomain::Scripted;
 check(!sameSceneContext(source,desired,false,false));
 check(sceneDomain(false,false,true,true)==SceneDomain::Scripted);
 check(sceneDomain(true,true,true,true)==SceneDomain::Inactive);
 check(sceneDomain(false,true,true,true)==SceneDomain::Cinematic);
 for(bool enabled:{false,true})for(bool symmetric:{false,true}){
  check(!allowLegacyGunUnshift(true,enabled,symmetric));
  check(allowLegacyGunUnshift(false,enabled,symmetric)==(enabled&&symmetric));
 }
 check(canTransition(Phase::Off,Phase::Observing));check(canTransition(Phase::Observing,Phase::Warming));check(canTransition(Phase::Warming,Phase::Ready));check(!canTransition(Phase::Off,Phase::Ready));check(canTransition(Phase::Ready,Phase::RestartAer));check(!canTransition(Phase::RestartAer,Phase::Observing));
 check(selectBackend(false,false,false)==Backend::Aer);check(selectBackend(false,true,false)==Backend::Aer);check(selectBackend(true,false,false)==Backend::Native);check(selectBackend(true,true,true)==Backend::Aer);check(allowFsr(Backend::Native));check(allowFsr(Backend::Aer));
 PairIdentity p{11,22,7,7,7,3,3,0,1,true,true,true};check(validPair(p));auto bad=p;bad.rightImage=11;check(!validPair(bad));bad=p;bad.rightFrame=6;check(!validPair(bad));bad=p;bad.rootFrame=8;check(!validPair(bad));bad=p;bad.currentGeneration=4;check(!validPair(bad));bad=p;bad.leftEye=1;check(!validPair(bad));bad=p;bad.finalPass=false;check(!validPair(bad));bad=p;bad.initialized=false;check(!validPair(bad));bad=p;bad.layoutsKnown=false;check(!validPair(bad));
 PresentWaitOwnership waits;check(waits.downstreamMustWait());check(!waits.submit(false));check(waits.downstreamMustWait());check(waits.submit(true));check(!waits.downstreamMustWait());check(!waits.submit(true));
 check(!canRetire(true,false,false));check(!canRetire(true,true,true));check(!canRetire(false,true,false));check(canRetire(true,true,false));check(!canReadMirror(true,false,true));check(!canReadMirror(true,true,false));check(canReadMirror(true,true,true));
 for(int bits=0;bits<8;bits++)check(canSeedStorage(bits&1,bits&2,bits&4)==(bits==1));
 check(!canCompleteFrame(true,true,true,false)); // allocation convergence is not input provenance
 check(!canCompleteFrame(true,true,false,true)); // a new mirror invalidates this frame
 check(!canCompleteFrame(true,false,true,true));
 check(!canCompleteFrame(false,true,true,true));
 check(canCompleteFrame(true,true,true,true));
 for(bool leftOnly:{false,true})for(bool prepMarker:{false,true})for(bool inputsOnly:{false,true})for(bool noReplay:{false,true})for(bool noInputs:{false,true}){
  const auto plan=eyeWorkPlan(leftOnly,prepMarker,inputsOnly,noReplay,noInputs);
  check(plan.renderRight==!leftOnly);
  check(plan.prepareStereo==(!leftOnly||prepMarker));
  check(plan.prepareMirrors==(!leftOnly||(prepMarker&&!inputsOnly)));
  check(plan.captureReplay==(!leftOnly||(prepMarker&&!(inputsOnly&&noReplay))));
  check(plan.captureInputPlans==(!leftOnly||(prepMarker&&!(inputsOnly&&(noReplay||noInputs)))));
  if(plan.renderRight||plan.prepareMirrors){check(plan.captureReplay);check(plan.captureInputPlans);}
 }
 // Sparse profiling must not turn every frame into an instrumented baseline.
 for(bool sparse:{false,true})for(bool continuous:{false,true}){
  int sampled=0;
  for(uint64_t frame=0;frame<1200;++frame){
   const bool selected=cpu::sparseDetailFrame(frame,sparse,continuous);
   sampled+=selected;
   check(cpu::detailFrame(frame,sparse,continuous)==(continuous||selected));
   if(selected)check(frame%120==60&&frame!=0);
  }
  check(sampled==(sparse&&!continuous?10:0));
 }
 // Diagnostic acceptance cannot masquerade as full stereo, accept a stale
 // final image, or publish before command recording/submission has completed.
 check(validLeftDiagnostic(true,11,11,7,7,7,true,true));
 check(!validLeftDiagnostic(false,11,11,7,7,7,true,true));
 check(!validLeftDiagnostic(true,0,0,7,7,7,true,true));
 check(!validLeftDiagnostic(true,11,22,7,7,7,true,true));
 check(!validLeftDiagnostic(true,11,11,0,0,0,true,true));
 check(!validLeftDiagnostic(true,11,11,7,6,7,true,true));
 check(!validLeftDiagnostic(true,11,11,7,7,6,true,true));
 check(!validLeftDiagnostic(true,11,11,7,7,7,false,true));
 check(!validLeftDiagnostic(true,11,11,7,7,7,true,false));
 auto diagnostic=p;diagnostic.rightImage=0;check(!validPair(diagnostic));
 // Final commands and pre-pass state must own argument bytes after their
 // source and temporary callable disappear or a later bind overwrites state.
 struct Replay {bool final{true},replaying{};std::vector<std::function<void()>> commands;std::map<uint64_t,std::function<void()>> state;} replay;

 // Descriptor reflection during deferred replay must see the executing
 // pipeline, including initial state and post-pass restoration.
 struct PipelineState {int pipeline{};} tracked;
 std::vector<int> pipelines;
 auto bindPipeline=[&](int pipeline){return [&,pipeline]{executeReplayPipeline(tracked,pipeline,[&]{pipelines.push_back(tracked.pipeline);});};};
 auto firstPipeline=bindPipeline(11),lastPipeline=bindPipeline(22);
 firstPipeline();lastPipeline();firstPipeline();check(tracked.pipeline==11);
 lastPipeline();check(tracked.pipeline==22);
 check(pipelines==std::vector<int>({11,22,11,22}));
 std::vector<int> seen;
 {
  std::vector<int> arguments{17,23};
  auto first=[payload=arguments,&seen]{seen.push_back(payload[0]);};
  recordReplayCommand(replay,1,first);
  arguments[0]=99;
 }
 auto initial=replay.state;
 recordReplayCommand(replay,1,[&seen]{seen.push_back(41);});
 check(seen==std::vector<int>({17,41}));
 replay.replaying=true;
 for(const auto& command:replay.commands)command();
 initial.at(1)();replay.state.at(1)();
 check(seen==std::vector<int>({17,41,17,41,17,41}));
 const auto captured=replay.commands.size();
 recordReplayCommand(replay,2,[&seen]{seen.push_back(53);},false);
 check(replay.commands.size()==captured&&replay.state.count(2)==0&&seen.back()==53);
 // Immediate execution observes the newly stored state, including re-entry.
 recordReplayCommand(replay,3,[&]{check(replay.state.count(3)==1);});
 // Batched calls stay batched, while overlapping updates retain the correct
 // per-binding state. Deferred commands must outlive caller arrays and later
 // state changes; batch payloads must never be built for ordinary recording.
 Replay batched;batched.final=false;
 std::vector<std::pair<uint32_t,std::vector<int>>> batches;
 int factories=0;
 auto bind=[&](uint32_t first,const std::vector<int>& values){
  recordReplayBatch(batched,0x40000,first,uint32_t(values.size()),
   [&](uint32_t i){int value=values[i];return [&,index=first+i,value]{batches.push_back({index,{value}});};},
   [&]{++factories;return [&,first,owned=values]{batches.push_back({first,owned});};},
   [&]{check(batched.state.count(0x40000+first)==1);batches.push_back({first,values});});
 };
 bind(2,{10,20,30,40});
 check(factories==0&&batches.size()==1&&batches.back().second.size()==4);
 auto beforeOverwrite=batched.state;
 bind(3,{21,31});
 check(batched.state.size()==4&&batches.size()==2);
 batches.clear();for(auto&[key,fn]:batched.state)fn();
 check(batches==decltype(batches)({{2,{10}},{3,{21}},{4,{31}},{5,{40}}}));
 batched.final=true;
 {std::vector<int> temporary{51,61};bind(4,temporary);temporary.assign(2,99);}
 bind(4,{52});
 check(factories==2&&batched.commands.size()==2);
 batches.clear();for(auto& fn:batched.commands)fn();
 check(batches==decltype(batches)({{4,{51,61}},{4,{52}}}));
 batches.clear();beforeOverwrite.at(0x40003)();batched.state.at(0x40004)();batched.state.at(0x40005)();
 check(batches==decltype(batches)({{3,{20}},{4,{52}},{5,{61}}}));
 batched.replaying=true;bind(2,{70,80,90});
 check(factories==2&&batched.commands.size()==2&&batches.back().second==std::vector<int>({70,80,90}));
 // Identical argument reuse must still execute binds, observe changed resource
 // contents, retain every final command and survive re-entry replacing state.
 Replay memo;memo.final=false;int resource=1;std::vector<int> observations;
 recordReplayCommand(memo,17,[&]{observations.push_back(resource);});
 resource=2;check(reuseReplayCommand(memo,17,true,[&]{observations.push_back(resource);}));
 check(observations==std::vector<int>({1,2})&&memo.commands.empty());
 check(!reuseReplayCommand(memo,18,true,[]{})&&!reuseReplayCommand(memo,17,false,[]{}));
 memo.final=true;
 check(reuseReplayCommand(memo,17,true,[&]{memo.state[17]=[&]{observations.push_back(99);};}));
 resource=3;memo.commands.back()();memo.state.at(17)();
 check(observations==std::vector<int>({1,2,3,99}));
 memo.replaying=true;check(reuseReplayCommand(memo,17,true,[]{}));check(memo.commands.size()==1);
 Replay cached;cached.final=false;std::map<uint64_t,std::pair<int,int>> values;
 int made=0,executed=0;std::vector<int> restored;
 auto cachedBind=[&](uint32_t first,const std::vector<std::pair<int,int>>& input){
  return recordReplayBatchCached(cached,values,0x40000,first,uint32_t(input.size()),
   [&](uint32_t i){return input[i];},
   [&](uint32_t i){++made;auto value=input[i];return [&,value]{restored.push_back(value.first+value.second);};},
   [&]{return [&,owned=input]{for(auto value:owned)restored.push_back(value.first+value.second);};},
   [&]{++executed;});
 };
 check(cachedBind(2,{{10,1},{20,2}})==0&&made==2);
 auto cachedInitial=cached.state;
 check(cachedBind(2,{{10,1},{20,2}})==2&&made==2&&executed==2);
 check(cachedBind(3,{{20,3}})==0&&made==3); // offset-only change
 check(cachedBind(2,{{11,1}})==0&&made==4); // buffer-only change
 cached.final=true;check(cachedBind(2,{{11,1},{20,3}})==2);
 check(cachedBind(2,{{11,1},{20,3}})==2&&cached.commands.size()==2);
 check(cachedBind(2,{{99,1}})==0); // old captured bytes survive overwrite
 restored.clear();cached.commands[0]();cachedInitial.at(0x40002)();
 check(restored==std::vector<int>({12,23,11}));
 cached.state.erase(0x40002);check(cachedBind(2,{{99,1}})==0); // stale memo cannot suppress state
 cached=Replay{};values.clear();cached.final=false;
 check(cachedBind(2,{{99,1}})==0); // new command buffer starts cold
 std::cout<<"Native stereo identity, backend and ownership failures="<<errors<<'\n';return errors?1:0;}
