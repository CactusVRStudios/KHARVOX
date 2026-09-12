#include "../src/native/NativePairWatchPolicy.h"
#include <cstdlib>
#include <iostream>
using namespace kharvox::native;
static void require(bool ok){if(!ok)std::abort();}
int main(){
 PairWatchMetrics a{},b{};require(!pairWatchChange(a,b));
 b.eyeGap[31]=19;require(pairWatchChange(a,b));require(pairWatchChange(b,a));
 b={};b.whiteFraction[1]=0.13f;require(pairWatchChange(a,b));require(!pairWatchChange(b,a));
 PairWatchTrigger t;for(int i=0;i<119;++i)require(!t.advance(false,false));
 require(!t.advance(false,false)&&t.pending);for(int i=0;i<6;++i)require(!t.advance(false,false));
 require(t.advance(false,false)&&t.events==1&&!t.pending);
 // A persistent candidate cannot fill disk every frame; the cooldown applies.
 for(int i=0;i<112;++i)require(!t.advance(true,false));
 require(!t.advance(true,false)&&t.pending);
 for(int i=0;i<7;++i)t.advance(false,false);require(t.events==2);
 // Manual requests still obey the event and in-flight burst bounds.
 for(int i=0;i<1000;++i)t.advance(false,true);require(t.events==32&&!t.pending);
 for(int i=0;i<1000;++i)require(!t.advance(true,true));
 std::cout<<"Pair watch trigger, pre/post history, cooldown and cap passed\n";
}
