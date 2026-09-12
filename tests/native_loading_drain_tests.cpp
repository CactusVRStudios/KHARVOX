#include "../src/native/NativeLoadingDrain.h"
#include "../src/native/NativeStereoPolicy.h"
#include <iostream>
using namespace kharvox::native;
int main(){
 int errors=0;auto check=[&](bool value){if(!value)++errors;};
 // Reproduce the r152 log: one unsubmitted original command, active pass,
 // attachment preservation already recorded. It must retain ownership until
 // the engine, submission and GPU completion have all reached the boundary.
 LoadingDrain drain;
 check(drain.begin(123,true,true,false));
 check(drain.active()&&drain.command()==123);
 check(!drain.begin(456,true,true,false)); // no overlapping transaction
 check(!drain.complete(true,true,true)); // FrameRoot has not returned
 check(drain.rootReturned());check(!drain.rootReturned());
 for(int bits=0;bits<7;++bits){
  check(!drain.complete(bits&1,bits&2,bits&4));
  check(drain.active()&&drain.command()==123); // no premature reuse/retirement
 }
 check(drain.complete(true,true,true));check(!drain.active());
 check(!drain.complete(true,true,true));
 // A closed/unobserved/multiple/already-submitted command or injected second
 // eye cannot be drained by this loading-only exception.
 for(int bits=0;bits<8;++bits){
  LoadingDrain candidate;
  const bool accepted=candidate.begin(42,bits&1,bits&2,bits&4);
  check(accepted==(bits==3));
 }
 LoadingDrain empty;check(!empty.begin(0,true,true,false));
 // A loading frame never supplies root/final identities, even if allocation
 // convergence and attachment provenance from the previous level are ready.
 check(!canCompleteFrame(true,false,true,true));
 PairIdentity loading{11,22,8,8,0,4,4,0,1,false,true,true};
 check(!validPair(loading));
 // Repeated load transitions cannot carry ownership across frames.
 for(uintptr_t frame=1;frame<1000;++frame){
  check(drain.begin(frame,true,true,false));check(drain.rootReturned());
  check(!drain.recordingFinished(false));check(drain.recordingFinished(true));
  check(drain.complete(true,true,true));
 }
 std::cout<<"Native loading drain failures="<<errors<<'\n';return errors?1:0;
}
