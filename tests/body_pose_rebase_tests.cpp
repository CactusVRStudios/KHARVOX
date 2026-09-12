#include "../src/camera/BodyPoseRebase.h"
#include <iostream>
#include <limits>
int main(){
 int failures=0;auto near=[&](float a,float b){if(std::abs(a-b)>0.0005f)++failures;};
 // A body catches up, overshoots, then settles after a head turn. World
 // orientation and room-scale translation must stay unchanged at every step.
 for(float reference:{0.f,175.f,-178.f})for(float current:{0.f,3.f,1.f,-2.f,179.f,-179.f}){
    float yaw=25,f=3,l=-2;
    kharvox::rebaseHeadToBody(reference,current,yaw,f,l);
    near(std::remainder(current+yaw-reference-25,360.f),0);
    const float a=reference*0.01745329251994329577f,b=current*0.01745329251994329577f;
    near(std::cos(a)*3-std::sin(a)*-2,std::cos(b)*f-std::sin(b)*l);
    near(std::sin(a)*3+std::cos(a)*-2,std::sin(b)*f+std::cos(b)*l);
 }
 float yaw=15,f=3,l=4;near(kharvox::rebaseHeadToBody(0,std::numeric_limits<float>::quiet_NaN(),yaw,f,l),0);near(yaw,15);near(f,3);near(l,4);
 std::cout<<"body pose rebase failures="<<failures<<'\n';return failures?1:0;
}
