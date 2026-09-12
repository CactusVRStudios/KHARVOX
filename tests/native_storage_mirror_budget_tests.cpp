#include "../src/native/NativeStorageMirrorBudget.h"
#include "../src/native/NativeFramebufferRetirement.h"
#include <map>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>
using namespace kharvox::native;
int main(){
 StorageMirrorBudget<> budget;
 // Exact allocation sizes of the 16 distinct resources in r163 PID 39744.
 const uint64_t observed[]={6144,81920,81920,81920,512,512,512,512,
     5505024,1572864,393216,98304,24576,1572864,393216,98304};
 for(size_t i=0;i<16;++i)assert(budget.reserve(i+1,observed[i]));
 assert(budget.count()==16&&budget.bytes()==9912320);
 assert(budget.reserve(17,24576)); // Another small image must not trip a lifetime-16 cap.
 assert(!budget.reserve(17,24576)); // Duplicate handle cannot double-charge.
 const auto held=budget.bytes();
 assert(!budget.reserve(18,std::numeric_limits<uint64_t>::max()));
 assert(budget.bytes()==held&&budget.count()==17);
 // Failed construction releases only its unpublished reservation.
 assert(budget.reserve(18,1024));assert(budget.release(18));
 assert(!budget.release(18)&&!budget.release(0)&&budget.bytes()==held);
 // Long sessions may create more than 16 mirrors when retired allocations
 // actually leave. A retained resource never loses its charge on a map reset.
 for(unsigned cycle=0;cycle<1000;++cycle){assert(budget.reserve(18,4096));assert(budget.release(18));}
 assert(budget.bytes()==held);
 StorageMirrorBudget<2,100> bounded;
 assert(!bounded.reserve(0,10)&&!bounded.reserve(1,0));
 assert(bounded.reserve(1,60));assert(!bounded.reserve(2,41));
 assert(bounded.reserve(2,40));assert(!bounded.reserve(3,1));
 assert(bounded.release(1)&&bounded.bytes()==40);
 assert(bounded.reserve(3,60)&&bounded.bytes()==100);
 StorageMirrorBudget<2,100> countBounded;
 assert(countBounded.reserve(1,1)&&countBounded.reserve(2,1));
 assert(!countBounded.reserve(3,1)&&countBounded.bytes()==2);

 // Reproduce view-before-framebuffer retirement during Medium -> Low.
 struct Info {std::vector<uint64_t> attachments;};
 struct Target {uint64_t framebuffer{};};
 std::map<uint64_t,Info> infos{{1,{{10,20}}},{2,{{10}}},{3,{{20}}},{4,{{10}}}};
 std::map<uint64_t,Target> targets{{1,{101}},{2,{102}},{3,{103}}};
 std::map<uint64_t,MirrorFramebufferReferences> refs{{10,{2}},{20,{2}}};
 std::vector<uint64_t> forgotten;
 auto forget=[&](uint64_t fb){forgotten.push_back(fb);};
 const auto retired=detachFramebuffersUsingView(uint64_t(10),infos,targets,refs,forget);
 assert((retired==std::vector<uint64_t>{101,102}));
 assert((forgotten==std::vector<uint64_t>{1,2,4}));
 assert(refs[10].count==0&&refs[20].count==1);
 assert(infos.size()==1&&targets.size()==1&&targets.at(3).framebuffer==103);
 // Later old-framebuffer destruction is a no-op, never a second decrement.
 assert(detachFramebuffersUsingView(uint64_t(10),infos,targets,refs,forget).empty());
 // A reused source view handle belongs only to the newly created framebuffer.
 infos[5]={{10}};targets[5]={105};refs[10].add();
 assert((detachFramebuffersUsingView(uint64_t(10),infos,targets,refs,forget)==std::vector<uint64_t>{105}));
 assert(refs[20].count==1&&targets.count(3)==1);
 // Losing a mirror owner also retires aliases, but a still-live original
 // alias framebuffer must retain metadata so a new mirror can be built.
 assert((detachFramebuffersUsingView(uint64_t(20),infos,targets,refs,forget,false)==std::vector<uint64_t>{103}));
 assert(infos.count(3)==1&&targets.empty()&&refs[20].count==0);

 MirrorFramebufferReferences storage;
 assert(!storage.inUse()); // Storage creation itself creates no framebuffer.
 storage.add();storage.add();
 storage.remove(false);assert(storage.count==2); // Destroying an unmirrored FB changes nothing.
 storage.remove(true);assert(storage.inUse());
 storage.remove(true);assert(!storage.inUse());
 storage.remove(true);assert(!storage.inUse()); // No underflow.
 MirrorFramebufferReferences attachment{1};assert(attachment.inUse());
 attachment.remove(true);assert(!attachment.inUse());
 // A source view with no outstanding Native frame and no remaining mirrored
 // framebuffer users can retire. Until then, its physical budget stays held.
 assert(budget.reserve(18,4096));storage.add();
 assert(storage.inUse()&&budget.bytes()==held+4096);
 storage.remove(true);assert(!storage.inUse());assert(budget.release(18));
 assert(budget.bytes()==held);
}
