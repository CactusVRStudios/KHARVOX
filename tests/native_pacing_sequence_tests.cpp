#include "../src/native/NativePacingSequence.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
using kharvox::native::cpu::PacingSequence;
struct Sample{uint64_t frame{},time{};};
int main(){
 PacingSequence<Sample,6,2> s;
 assert(!s.push({1,1},true));
 assert(!s.push({2,2},false)); // interrupted warmup starts over
 assert(!s.push({3,3},true));assert(!s.push({4,4},true));
 for(uint64_t f=5;f<11;++f)assert(s.push({f,f%2?200ull:100ull},true)==(f==10));
 assert(s.size()==6);
 for(size_t i=0;i<s.size();++i){assert(s[i].frame==i+5);assert(s[i].time==((i+5)%2?200:100));}
 assert(!s.push({11,900},true));assert(!s.seal());assert(s[5].frame==10);
 PacingSequence<Sample,8,0> partial;
 Sample input{100,42};assert(!partial.push(input,true));input.time=900;
 assert(!partial.push({103,55},true)); // preserve serial gap, do not fabricate frames
 assert(partial.push({},false));assert(partial.size()==2);
 assert(partial[0].time==42&&partial[1].frame==103);
 assert(!partial.push({104,66},true));assert(!partial.seal());
 PacingSequence<Sample,8,0> teardown;
 assert(!teardown.seal());assert(!teardown.push({7,8},true));
 assert(teardown.seal());assert(!teardown.seal());assert(teardown.size()==1);
}
