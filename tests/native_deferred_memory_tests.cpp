#include "../src/native/NativeDeferredMemory.h"
#include "../src/native/NativeLoadingDrain.h"
#include <atomic>
#include <condition_variable>
#include <thread>
#include <vector>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

int main(){
 using Queue=kharvox::native::DeferredMemoryFreeQueue<int,2>;
 using Result=Queue::Result;
 Queue queue;bool outstanding=false,gpuDone=false,cpuRetired=false;
 std::vector<int> released;
 auto free=[&](int handle){
  if(outstanding)assert(gpuDone&&cpuRetired);
  released.push_back(handle);
 };
 assert(queue.release(1,false,free)==Result::Released);
 assert(queue.begin([&]{outstanding=true;}));
 assert(!queue.begin([]{assert(false);}));
 assert(queue.release(2,true,free)==Result::Deferred);
 assert(queue.release(2,true,free)==Result::Refused); // double free stays rejected
 assert(queue.release(3,false,free)==Result::Refused); // callback lifetime unknown
 assert(queue.release(3,true,free)==Result::Deferred);
 assert(queue.release(4,true,free)==Result::Refused); // bounded, no dropped entry
 assert(released==std::vector<int>{1});
 gpuDone=true;cpuRetired=true;
 assert(queue.complete(free,[&]{outstanding=false;})==2);
 assert((released==std::vector<int>{1,2,3})&&!outstanding);
 assert(queue.complete(free,[]{})==0); // no repeated physical frees
 assert(queue.begin([&]{outstanding=true;}));
 assert(queue.release(4,true,free)==Result::Deferred);
 assert(queue.complete(free,[&]{outstanding=false;})==1);
 assert(released.back()==4);

 // A runtime thread can enqueue while the owner is waiting for it. It must
 // return without waiting for frame completion (xrEndFrame worker dependency).
 assert(queue.begin([&]{outstanding=true;}));
 std::thread runtime([&]{assert(queue.release(5,true,free)==Result::Deferred);});
 runtime.join();assert(released.back()==4);
 assert(queue.complete(free,[&]{outstanding=false;})==1);

 // Beginning the next frame cannot race an immediate physical free. Coordinate
 // the actual release callback, without sleeps or a timing-dependent assertion.
 std::mutex mutex;std::condition_variable cv;bool freeing=false,mayFinish=false;
 std::atomic_bool freeDone=false;
 std::thread releaser([&]{
  assert(queue.release(6,false,[&](int){
   std::unique_lock lock(mutex);freeing=true;cv.notify_all();
   cv.wait(lock,[&]{return mayFinish;});freeDone=true;
  })==Result::Released);
 });
 {std::unique_lock lock(mutex);cv.wait(lock,[&]{return freeing;});}
 std::thread owner([&]{assert(queue.begin([&]{assert(freeDone);outstanding=true;}));});
 {std::lock_guard lock(mutex);mayFinish=true;cv.notify_all();}
 releaser.join();owner.join();
 assert(queue.complete(free,[&]{outstanding=false;})==0);

 // r159 regression: a loading boundary with zero injected commands must not
 // drop only outstanding and strand an active free queue before the next root.
 kharvox::native::EmptyLoadingDrain emptyDrain;
 for(int frame=0;frame<3;++frame){
  gpuDone=cpuRetired=false;
  assert(queue.begin([&]{outstanding=true;}));
  const auto previous=released.size();
  if(frame!=1)assert(queue.release(7+frame,true,free)==Result::Deferred);
  assert(emptyDrain.begin(true,true));
  assert(!emptyDrain.complete(true,true)); // original root still executing
  assert(emptyDrain.rootReturned());
  assert(!emptyDrain.rootReturned());
  assert(!emptyDrain.complete(false,true)); // a later native command invalidates it
  assert(!emptyDrain.complete(true,false)); // GPU completion failed
  assert(emptyDrain.active()&&outstanding&&released.size()==previous);
  assert(!queue.begin([]{assert(false);})); // cannot start another owner yet
  gpuDone=cpuRetired=true;
  assert(emptyDrain.complete(true,true));
  assert(queue.complete(free,[&]{outstanding=false;})==std::size_t(frame!=1));
  assert(!emptyDrain.active()&&!outstanding);
 }
 for(int bits=0;bits<3;++bits){
  kharvox::native::EmptyLoadingDrain rejected;
  assert(!rejected.begin(bits&1,bits&2));
 }
}
