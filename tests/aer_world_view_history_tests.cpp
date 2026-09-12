#include "../src/camera/AerWorldViewHistory.h"
#include <cstdlib>
#include <thread>
void check(bool ok){if(!ok)std::abort();}
int main(){
    using namespace kharvox;
    auto view=[](uint64_t id,uint64_t frame,int eye,float x){
        AerWorldView v;v.pose={x,2,3,1,0,0,0,1,0,0,0,1};v.fov={100,80};
        v.poseId=id;v.present=frame;v.eye=eye;v.level=2;v.context=42;return v;};
    AerWorldViewHistory h;auto old=view(10,8,0,1),current=view(12,9,1,2);
    std::thread producer([&]{h.remember(old);h.remember(current);});producer.join();
    AerWorldView source,target;
    check(h.recognize(old.pose.data(),old.fov.data(),9,2,source)==AerWorldViewResult::Unchanged);
    check(source.poseId==10&&source.eye==0);
    check(h.recognize(old.pose.data(),old.fov.data(),12,2,source)==AerWorldViewResult::UnknownSource);
    check(h.recognize(old.pose.data(),old.fov.data(),9,3,source)==AerWorldViewResult::UnknownSource);
    AerWorldViewHistory indistinguishable;
    indistinguishable.remember(old);auto sameImageNewInput=old;sameImageNewInput.poseId=14;
    indistinguishable.remember(sameImageNewInput);
    check(indistinguishable.recognize(old.pose.data(),old.fov.data(),9,2,source)==AerWorldViewResult::AmbiguousSource);
    auto resolve=[&](const AerWorldView& input,uint64_t wanted,int eye,uint64_t now=9,uint64_t level=2){
        return h.align(input.pose.data(),input.fov.data(),wanted,eye,now,level,source,target);};
    check(resolve(old,12,1)==AerWorldViewResult::Updated);
    check(source.poseId==10&&target.poseId==12&&target.eye==1&&target.pose==current.pose);
    check(resolve(current,12,1)==AerWorldViewResult::Unchanged);
    check(resolve(old,14,1)==AerWorldViewResult::MissingTarget);
    check(resolve(old,12,1,7)==AerWorldViewResult::UnknownSource); // future observations
    check(resolve(old,12,1,12)==AerWorldViewResult::UnknownSource); // stale
    check(resolve(old,12,1,9,3)==AerWorldViewResult::UnknownSource); // level transition
    auto unknown=old;unknown.pose[0]+=.00001f;
    check(resolve(unknown,12,1)==AerWorldViewResult::UnknownSource); // no fuzzy matching
    unknown=old;unknown.fov[0]=90;
    check(resolve(unknown,12,1)==AerWorldViewResult::UnknownSource); // another projection
    auto different=current;different.context=55;different.poseId=15;h.remember(different);
    check(resolve(old,15,1)==AerWorldViewResult::MissingTarget); // another camera
    auto duplicate=old;duplicate.context=99;h.remember(duplicate);
    check(resolve(old,12,1)==AerWorldViewResult::AmbiguousSource);
    AerWorldViewHistory invalid;auto bad=old;bad.pose[3]=2;invalid.remember(bad);
    check(invalid.align(bad.pose.data(),bad.fov.data(),12,1,9,2,source,target)==AerWorldViewResult::UnknownSource);
}
