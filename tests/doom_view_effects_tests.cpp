#include "../src/camera/DoomViewEffects.h"
#include <cassert>
#include <vector>
int main(){
    std::vector<unsigned char> image(kharvox::viewKicksValueRva+sizeof(LONG));
    auto shakes=reinterpret_cast<LONG*>(image.data()+kharvox::viewShakesValueRva);
    auto kicks=reinterpret_cast<LONG*>(image.data()+kharvox::viewKicksValueRva);
    assert(kharvox::enforceViewEffects(image.data(),image.size())==4&&*shakes==0&&*kicks==0);
    std::memcpy(image.data()+0x22b7668,"view_skipShakes",sizeof("view_skipShakes"));
    std::memcpy(image.data()+0x22b76e8,"view_skipKicks",sizeof("view_skipKicks"));
    const unsigned char guard[]{0x44,0x39,0x25,0xbd,0x3c,0xd6,0x04,0x75,0x40};
    std::memcpy(image.data()+0xe6c30c,guard,sizeof(guard));
    assert(kharvox::enforceViewEffects(image.data(),image.size())==3&&*shakes==1&&*kicks==1);
    assert(kharvox::enforceViewEffects(image.data(),image.size())==0);
    *shakes=0;assert(kharvox::enforceViewEffects(image.data(),image.size())==1&&*shakes==1);
    *kicks=0;*shakes=2;
    assert(kharvox::enforceViewEffects(image.data(),image.size())==4&&*shakes==2&&*kicks==0);
    *shakes=0;image[0xe6c30c]^=1;
    assert(kharvox::enforceViewEffects(image.data(),image.size())==4&&*shakes==0&&*kicks==0);
    assert(kharvox::enforceViewEffects(image.data(),64)==4);
    assert(kharvox::enforceViewEffects(nullptr,0)==4);
}
