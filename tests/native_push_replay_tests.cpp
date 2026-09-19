#include "../src/native/NativePushReplay.h"
#include <array>
#include <cassert>
#include <functional>
#include <map>
#include <random>

using State=std::map<std::pair<unsigned,unsigned>,std::pair<VkPipelineLayout,uint32_t>>;
static void apply(State& state,VkPipelineLayout layout,VkShaderStageFlags stages,uint32_t offset,uint32_t size,const void* data){
 const auto words=static_cast<const uint32_t*>(data);
 for(unsigned bit=0;bit<6;++bit)if(stages&(1u<<bit))
  for(unsigned word=0;word<size/4;++word)state[{bit,offset/4+word}]={layout,words[word]};
}
struct Replay {
 kharvox::native::NativePushReplay pushState;
 std::vector<std::function<void()>> commands;
 bool final{},replaying{};
};

int main(){
 using namespace kharvox::native;
 const auto layout=reinterpret_cast<VkPipelineLayout>(uintptr_t(1));
 const auto other=reinterpret_cast<VkPipelineLayout>(uintptr_t(2));
 NativePushReplay pushes;
 const void* retained{};
 for(uint32_t value=0;value<10000;++value){
  pushes.write(layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,4,&value);
  assert(pushes.size()==1);
  pushes.replay([&](auto l,auto stages,auto offset,auto size,const void* data){
   assert(l==layout&&stages==(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT)&&offset==0&&size==4);
   assert(*static_cast<const uint32_t*>(data)==value);
   assert(!retained||retained==data);retained=data;
  });
 }
 State expected;
 std::mt19937 random(719);
 pushes={};
 for(unsigned trial=0;trial<1000;++trial){
  if(trial%37==0){pushes={};expected.clear();}
  const auto l=random()%2?layout:other;
  const unsigned first=random()%48,count=1+random()%16,stages=1+random()%63;
  std::array<uint32_t,16> words{};for(auto& word:words)word=random();
  pushes.write(l,stages,first*4,count*4,words.data());
  apply(expected,l,stages,first*4,count*4,words.data());
  State observed;
  pushes.replay([&](auto a,auto b,auto c,auto d,const void* e){apply(observed,a,b,c,d,e);});
  assert(observed==expected);
 }
 Replay replay;
 State live;
 const auto emit=[&](auto a,auto b,auto c,auto d,const void* e){apply(live,a,b,c,d,e);};
 std::array<uint32_t,4> words{1,2,3,4};
 recordNativePush(replay,layout,17,0,16,words.data(),emit);
 assert(replay.commands.empty());
 const auto initial=replay.pushState;
 const auto initialState=live;
 replay.final=true;
 recordNativePush(replay,other,1,4,4,words.data(),emit);
 words[0]=99;
 recordNativePush(replay,layout,17,8,4,words.data(),emit);
 const auto finalState=live;
 assert(replay.commands.size()==2);
 words.fill(0);live.clear();
 initial.replay(emit);
 assert(live==initialState);
 for(const auto& command:replay.commands)command();
 assert(live==finalState);
 live.clear();replay.pushState.replay(emit);
 assert(live==finalState);
 for(const auto offset:{1u,65536u,UINT32_MAX}){
  bool rejected{};
  try{replay.pushState.write(layout,1,offset,4,words.data());}catch(const std::runtime_error&){rejected=true;}
  assert(rejected);
 }
}
