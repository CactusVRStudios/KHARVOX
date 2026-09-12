#include "../src/native/NativeDescriptorReuse.h"
#include <cstdlib>
#include <iostream>
using namespace kharvox::native;
static void require(bool ok){if(!ok)std::abort();}
int main(){
 DescriptorReuseKey base{100,200,300};descriptorKeyBuffer(base,0,6,400,256,512);descriptorKeyImage(base,1,1,500,600,5);
 auto changed=[&](size_t at){auto copy=base;++copy[at];require(copy!=base);};
 // Source/effective-set/layout identity, buffer type/handle/offset/range and
 // image view/sampler/layout must all invalidate reuse, including same-frame writes.
 for(size_t i=0;i<base.size();++i)changed(i);
 DescriptorReuseKey same{100,200,300};descriptorKeyBuffer(same,0,6,400,256,512);descriptorKeyImage(same,1,1,500,600,5);require(base==same);
 // An eye-specific replacement remains different even with the same source.
 auto left=base,right=base;descriptorKeyBuffer(left,0,6,1000,256,512);descriptorKeyBuffer(right,0,6,1001,256,512);require(left!=right);
 std::cout<<"Descriptor full-content identity and eye isolation passed\n";
}
