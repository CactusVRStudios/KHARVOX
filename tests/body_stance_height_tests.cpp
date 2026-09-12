#include "../src/camera/BodyStanceHeight.h"
#include <cstdlib>
#include <limits>
void check(bool v){if(!v)std::abort();}
int main(){
 kharvox::BodyStanceHeight h;
 unsigned frame=0;float value=87;
 auto sample=[&](float measured,float body=0,bool button=false){
  return value=h.update(measured,value,false,button,++frame,body);};
 check(h.update(87,87,true,false,0,0)==87);
 // A short press/release may precede the actual crouch animation.
 check(sample(87,0,true)==87);check(sample(87)==87);
 check(sample(80)==87);check(sample(70)==70);sample(60);check(sample(55)==55);
 for(int i=0;i<5;++i)check(sample(55)==55);
 check(sample(55.5f)==55);
 // Released input must not force a blocked standing pose.
 for(int i=0;i<5;++i)check(sample(55)==55);
 sample(65);sample(75);sample(85);sample(87);check(sample(87)==87);
 // Camera moves first, physics follows: do not accept the first jump spike.
 check(sample(98)==87);
 check(sample(76,10)==87);check(sample(98,20)==87);
 check(sample(76,30)==87);check(sample(87,30)==87);
 check(sample(87,30)==87);check(sample(87,30)==87);
 check(sample(100,20)==87);check(sample(72,0)==87);
 for(int i=0;i<4;++i)check(sample(87,0)==87);
 // Single-frame landing offset and opposite-sign jitter remain rejected.
 check(sample(67)==87);check(sample(87)==87);
 for(int i=0;i<8;++i)check(sample(i%2?96:78)==87);
 check(h.update(55,87,false,true,frame,0)==87); // repeated eye/camera call
 check(h.update(55,55,true,true,++frame,0)==55);value=55;
 check(sample(70)==55);check(sample(87)==87);
 check(sample(std::numeric_limits<float>::infinity())==87);
}
