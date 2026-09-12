#pragma once
#include <cstdint>
namespace kharvox::native {
inline const char* freshShadowValue(bool enabled,uintptr_t objectRva){
 if(!enabled)return nullptr;
 if(objectRva==0x6c0b830)return "0,0,0,0,0";
 if(objectRva==0x6eac480)return "0,0,0,0";
 return nullptr;
}
}
