#include "../src/native/NativeSnapshotBatch.h"
#include <cstdlib>
using Cache=kharvox::native::SnapshotBatchCache<uint64_t,uint64_t,4,2>;
static void check(bool value){if(!value)std::abort();}
int main(){
 Cache cache;std::array<uint64_t,8> context{2,3,4,5,0,0,1,99};std::array<uint64_t,2> source{10,11},effective{20,21};
 auto key=Cache::key(context,source,effective);check(key.has_value());Cache::Result resolved{30,31};
 auto first=cache.begin(1,key);check(!first.hit&&cache.publish(1,first.token,*key,resolved));
 source[0]=50;effective[0]=60;check(key->source[0]==10&&key->effective[0]==20); // owned caller arguments
 auto same=cache.begin(1,key);check(same.hit&&(*same.hit)[0]==30);check(cache.publish(1,same.token,*key,*same.hit));
 // New dynamic offsets are execution arguments, never borrowed from the cache.
 uint32_t forwardedOffset{},calls{};auto forward=[&](uint32_t offset){auto a=cache.begin(1,key);check(a.hit.has_value());++calls;forwardedOffset=offset;cache.publish(1,a.token,*key,*a.hit);};
 forward(256);forward(512);check(calls==2&&forwardedOffset==512);
 // Each policy dependency invalidates the result independently.
 for(size_t i=0;i<context.size();++i){auto changed=*key;++changed.context[i];auto miss=cache.begin(1,changed);check(!miss.hit);cache.publish(1,miss.token,*key,resolved);}
 auto changed=*key;changed.source[0]++;check(!cache.begin(1,changed).hit);
 auto repopulate=cache.begin(1,key);cache.publish(1,repopulate.token,*key,resolved);
 changed=*key;changed.effective[1]++;check(!cache.begin(1,changed).hit);
 // A different/unsupported bind in this command buffer supersedes its storage masks.
 repopulate=cache.begin(1,key);cache.publish(1,repopulate.token,*key,resolved);cache.begin(1,{});check(!cache.begin(1,key).hit);
 // Re-entrant recording and lifecycle changes cannot publish stale outer results.
 auto outer=cache.begin(1,key);auto inner=cache.begin(1,changed);check(cache.publish(1,inner.token,changed,resolved));check(!cache.publish(1,outer.token,*key,resolved));
 outer=cache.begin(1,key);cache.invalidate(1);check(!cache.publish(1,outer.token,*key,resolved));
 outer=cache.begin(1,key);cache.clear();inner=cache.begin(1,key);check(!cache.publish(1,outer.token,*key,resolved));check(cache.publish(1,inner.token,*key,resolved));
 auto other=cache.begin(2,key);check(!other.hit);check(cache.publish(2,other.token,*key,resolved));
 auto full=cache.begin(3,key);check(!full.token&&!full.hit&&!cache.publish(3,full.token,*key,resolved));
 check(cache.begin(1,key).hit.has_value()); // capacity does not evict a live result
 std::array<uint64_t,5> large{1,2,3,4,5};check(!Cache::key(context,large,large));check(!Cache::key(context,std::span<const uint64_t>{},std::span<const uint64_t>{}));
 check(!Cache::key(context,source,std::span<const uint64_t>(effective.data(),1)));
}
