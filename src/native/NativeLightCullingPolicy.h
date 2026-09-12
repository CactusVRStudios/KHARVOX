#pragma once
#include <string>

namespace kharvox::native {
struct LightCullingValue { std::string text; int integer{}; };

// Own only the Native gameplay override. Reads and writes remain with the
// engine adapter; a denied write never counts as an activated correction.
struct LightCullingScope {
 bool held{};
 LightCullingValue restore;
 template<class Read, class Write>
 bool update(bool nativeGameplay, Read read, Write write) {
  if(!nativeGameplay&&!held)return true;
  LightCullingValue current;
  if(!read(current))return false;
  if(nativeGameplay){
   if(!held||current.integer!=1)restore=current;
   held=true;
   if(current.integer!=1){
    write("1");
    if(!read(current)||current.integer!=1)return false;
   }
   return true;
  }
  // Respect an external change made after our last frame instead of
  // overwriting it with an older value when leaving gameplay.
  if(current.integer==1&&restore.integer!=1){
   write(restore.text.c_str());
   if(!read(current)||current.integer!=restore.integer)return false;
  }
  held=false;
  return true;
 }
};
}
