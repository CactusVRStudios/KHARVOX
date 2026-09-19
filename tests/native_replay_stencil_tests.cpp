#include "../src/native/NativeReplayRecording.h"
#include "../src/native/NativeEyeBindings.h"
#include <array>
#include <cassert>
#include <functional>
#include <map>
#include <set>
#include <vector>

struct Replay {
 std::map<uint64_t,std::function<void()>> state;
 std::vector<std::function<void()>> commands;
 bool final{},replaying{};
};

int main(){
 using namespace kharvox::native;
 for(const auto base:{0x50000ull,0x60000ull,0x70000ull}){
  Replay replay;
  std::array<uint32_t,2> live{};
  uint32_t calls{},value{};
  const auto bind=[&](uint32_t faces,uint32_t v){
   recordReplayFaces(replay,base,faces,[&,v](uint32_t face){
    ++calls;
    if(face&1)live[0]=v;
    if(face&2)live[1]=v;
   });
  };
  for(const uint32_t faces:{3,1,2,3,2,1}){
   const auto before=calls;
   bind(faces,++value);
   assert(calls==before+1&&replay.state.size()==2);
   const auto expected=live;
   live={};std::set<int> seen;
   assert(restoreEyeBindingsOnce(1,seen,replay.state));
   assert(live==expected);
   assert(!restoreEyeBindingsOnce(1,seen,replay.state));
  }
  std::vector<std::function<void()>> initial;
  for(const auto&[key,command]:replay.state)initial.push_back(command);
  std::vector<std::array<uint32_t,2>> draws;
  replay.final=true;
  for(const uint32_t faces:{1,3,2,1}){
   bind(faces,++value);
   recordReplayCommand(replay,0,[&]{draws.push_back(live);},false);
  }
  const auto expectedDraws=draws;
  const auto expectedState=live;
  assert(replay.commands.size()==8);
  draws.clear();live={};replay.replaying=true;
  for(const auto& command:initial)command();
  for(const auto& command:replay.commands)command();
  assert(draws==expectedDraws&&live==expectedState);
  live={};
  for(const auto&[key,command]:replay.state)command();
  assert(live==expectedState);
 }
}
