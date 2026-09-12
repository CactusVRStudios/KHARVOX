#include "../src/native/NativeQualityTransition.h"
#include <cassert>
int main(){
 using kharvox::native::QualityTransition;
 QualityTransition q;
 assert(q.epoch()==0&&!q.presented(0,true));
 assert(q.begin());auto e=q.epoch();
 assert(!q.presented(e,true));assert(!q.presented(e,true));
 // A later allocation in the same preset invalidates prior stability.
 q.mutation(true);assert(!q.presented(e,true));
 auto during=q.epoch();assert(!q.presented(during,true));
 q.mutation(false);e=q.epoch();
 assert(!q.presented(e,true));assert(!q.presented(e,false));
 assert(!q.presented(e,true));assert(!q.presented(e,true));assert(q.presented(e,true));
 assert(q.epoch()==0&&!q.presented(e,true));
 // Repeated quality changes restart the transition, not the GPU drain.
 assert(q.begin());e=q.epoch();assert(!q.presented(e,true));
 assert(!q.begin());assert(!q.presented(e,true));e=q.epoch();
 assert(!q.presented(e,true));assert(!q.presented(e,true));assert(q.presented(e,true));
 // Resource mutation already in progress when the preset is detected.
 q.mutation(true);assert(q.begin());e=q.epoch();
 for(int i=0;i<5;++i)assert(!q.presented(e,true));
 q.mutation(false);e=q.epoch();
 assert(!q.presented(e,true));assert(!q.presented(e,true));assert(q.presented(e,true));
}
