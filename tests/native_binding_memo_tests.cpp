#include "../src/native/NativeBindingMemo.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <thread>
using namespace kharvox::native;
static void require(bool ok){if(!ok)std::abort();}
int main(){
 BindingMutationClock clock;using Key=std::array<uint64_t,8>;
 BindingMemo<Key,int,2> memo;Key key{1,2,3,4,5,6,0,8};auto first=clock.stamp();
 memo.put(key,17,first,clock.stamp());require(memo.find(key,first)&&*memo.find(key,first)==17);
 for(size_t field=0;field<key.size();++field){auto other=key;++other[field];require(!memo.find(other,first));}
 {BindingMutationScope update(clock);require(!clock.stamp());require(!memo.find(key,clock.stamp()));
  {BindingMutationScope nested(clock);require(!clock.stamp());}
  require(!clock.stamp());memo.put(key,90,first,clock.stamp());
 }
 require(!memo.find(key,clock.stamp()));
 memo.put(key,21,first,clock.stamp());require(!memo.find(key,clock.stamp()));
 const auto second=clock.stamp();memo.put(key,22,second,second);require(*memo.find(key,second)==22);
 std::atomic<int> stage{};
 std::thread writer([&]{BindingMutationScope update(clock);stage.store(1);while(stage.load()!=2)std::this_thread::yield();});
 while(stage.load()!=1)std::this_thread::yield();require(!clock.stamp());
 {BindingMutationScope overlapping(clock);require(!clock.stamp());stage.store(2);writer.join();require(!clock.stamp());}
 require(!memo.find(key,clock.stamp()));
 auto now=clock.stamp();auto other=key;++other[0];auto third=key;third[0]+=2;
 memo.put(other,31,now,now);memo.put(third,32,now,now);require(memo.size()==2&&!memo.find(third,now));
 memo.clear();require(!memo.find(other,now)&&memo.size()==0);
 memo.put(key,40,now,now);require(*memo.find(key,now)==40);
 try{BindingMutationScope throws(clock);throw 1;}catch(int){}require(clock.stamp()&&!memo.find(key,clock.stamp()));
 // Direct slots are hints: force collisions and verify full key/revision,
 // in-place updates, frame reset, and invalid writer stamps on every hit.
 BindingMemo<Key,int,2> fast;auto stamp=clock.stamp();bool direct{};
 fast.put(key,10,stamp,stamp);fast.put(other,20,stamp,stamp);
 require(*fast.findFast(key,stamp,0,direct)==10&&!direct);
 require(*fast.findFast(key,stamp,0,direct)==10&&direct);
 require(*fast.findFast(other,stamp,0,direct)==20&&!direct);
 require(*fast.findFast(key,stamp,0,direct)==10&&!direct);
 for(size_t field=0;field<key.size();++field){auto changed=key;changed[field]+=100;require(!fast.findFast(changed,stamp,0,direct)&&!direct);}
 fast.put(key,11,stamp,stamp);require(*fast.findFast(key,stamp,0,direct)==11&&direct);
 {BindingMutationScope mutation(clock);require(!fast.findFast(key,clock.stamp(),0,direct)&&!direct);}
 require(!fast.findFast(key,clock.stamp(),0,direct));
 stamp=clock.stamp();fast.put(key,12,stamp,stamp);require(*fast.findFast(key,stamp,0,direct)==12);
 fast.put(third,30,stamp,stamp);require(!fast.findFast(third,stamp,0,direct));
 fast.clear();require(!fast.findFast(key,stamp,0,direct));
 fast.put(key,99,stamp,stamp);require(*fast.findFast(key,stamp,0,direct)==99&&!direct);
 require(*fast.findFast(key,stamp,0,direct)==99&&direct);
 BindingMemo<Key,std::optional<uint64_t>,2> classes;
 classes.put(key,std::nullopt,stamp,stamp);auto invalid=classes.findFast(key,stamp,bindingMemoBucket(key),direct);
 require(invalid&&!invalid->has_value()); // invalid reflection is not a valid class
 classes.put(key,uint64_t(7),stamp,stamp);require(classes.findFast(key,stamp,bindingMemoBucket(key),direct)->value()==7);
 std::cout<<"Binding memo: key separation, mutation/reentry/concurrency, stale handles, cap and frame reset passed\n";
}
