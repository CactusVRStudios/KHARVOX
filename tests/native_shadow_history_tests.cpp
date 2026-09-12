#include "../src/native/NativeShadowHistoryPolicy.h"
#include "../src/native/NativeSharedShadowPolicy.h"
#include "../src/native/NativeFreshShadowPolicy.h"
#include <vector>
#include <cstdlib>
#include <iostream>
using namespace kharvox::native;
static void require(bool ok){if(!ok)std::abort();}
int main(){
 require(!freshShadowValue(false,0x6c0b830));
 require(!freshShadowValue(true,0x6c0b831));
 require(!freshShadowValue(true,0x66da780)); // quality/atlas sizes remain native
 require(std::strcmp(freshShadowValue(true,0x6c0b830),"0,0,0,0,0")==0);
 require(std::strcmp(freshShadowValue(true,0x6eac480),"0,0,0,0")==0);
 for(auto size:{1024u,49152u,65520u,65536u})require(sharedLightingSnapshotEligible(size,true,false));
 require(!sharedLightingSnapshotEligible(160,true,false)); // camera/dynamic blocks
 require(!sharedLightingSnapshotEligible(65519,true,false));
 require(sharedLightingSnapshotEligible(24576,false,true));
 require(sharedLightingSnapshotEligible(3145728,false,true));
 require(!sharedLightingSnapshotEligible(3145728,false,false)); // writable/unproven
 require(!sharedLightingSnapshotEligible(4194304,false,true)); // unknown list shape
 std::vector<unsigned char> left(65520),right(65520);
 require(sharedShadowTableCompatible(left,right));
 right[64]=17;right[79]=23;right[65519]=42;
 require(sharedShadowTableCompatible(left,right)); // changed atlas addressing
 right[80]=1;require(!sharedShadowTableCompatible(left,right));
 right[80]=0;right[65503]=1;require(!sharedShadowTableCompatible(left,right));
 right.resize(65519);require(!sharedShadowTableCompatible(left,right));
 require(shadowHistoryShape(8192,8192,true,1,1,true,true,true));
 require(shadowHistoryShape(8192,16384,true,1,1,true,true,true));
 require(!shadowHistoryShape(3072,1728,true,1,1,true,true,true));
 require(!shadowHistoryShape(8192,8192,false,1,1,true,true,true));
 require(!shadowHistoryShape(8192,8192,true,2,1,true,true,true));
 require(!shadowHistoryShape(8192,8192,true,1,2,true,true,true));
 require(!shadowHistoryShape(8192,8192,true,1,1,false,true,true));
 require(!shadowHistoryShape(8192,8192,true,1,1,true,false,true));
 require(!shadowHistoryShape(8192,8192,true,1,1,true,true,false));
 ShadowHistoryFrame s;require(!s.record(1,true,true));require(s.begin(1));
 require(!s.record(2,true,true));require(!s.record(1,false,true));require(!s.record(1,true,false));
 require(s.record(1,true,true));require(!s.record(1,true,true));require(!s.begin(2));
 require(!s.finish(false));require(!s.begin(2));require(s.finish(true));require(s.begin(2));
 require(s.begin(3)); // disabled frames contain no pending GPU work
 std::cout<<"Shadow history shape, frame identity, ordering and one-use checks passed\n";
}
