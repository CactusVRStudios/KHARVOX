#include "../src/native/NativeResourceSubset.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <set>
using kharvox::native::ResourceSubset;
static void require(bool value){if(!value)std::abort();}
int main(){
 ResourceSubset<uint64_t> index;std::map<uint64_t,bool> resources;
 auto check=[&]{std::set<uint64_t> expected;for(auto [key,selected]:resources)if(selected)expected.insert(key);require(expected==index.selected());};
 // The production case: a large vertex/index pool and few storage buffers.
 for(uint64_t i=1;i<=17000;++i){bool selected=i<=12;resources[i]=selected;index.record(i,selected);}
 check();require(index.selected().size()==12);
 index.record(1,false);resources[1]=false;check(); // handle changes usage
 index.erase(2);resources.erase(2);check();
 index.record(2,true);resources[2]=true;check(); // destroy/recreate
 index.record(2,true);check(); // repeated observation cannot duplicate a copy
 std::mt19937 random(144);
 for(int i=0;i<5000;++i){uint64_t key=1+random()%17000;const auto action=random()%3;
  if(action==0){index.erase(key);resources.erase(key);}else{bool selected=action==2;index.record(key,selected);resources[key]=selected;}
  if(i%100==0)check();
 }
 check();index.clear();resources.clear();check();
 std::cout<<"Resource subset matches full filtering across allocation, usage changes, destruction, handle reuse and reset\n";
}
