#include "../src/native/NativeSnapshotMemoryPolicy.h"
#include <iostream>
int main(){
 VkPhysicalDeviceMemoryProperties m{};m.memoryTypeCount=4;
 constexpr auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
 m.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
 m.memoryTypes[1].propertyFlags=host;
 m.memoryTypes[2].propertyFlags=host|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
 m.memoryTypes[3].propertyFlags=host|VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
 using kharvox::native::snapshotMemoryType;
 if(snapshotMemoryType(m,15,false)!=1||snapshotMemoryType(m,15,true)!=3){std::cerr<<"CPU compatibility / local GPU preference failed";return 1;}
 if(snapshotMemoryType(m,7,true)!=1||snapshotMemoryType(m,4,true)!=2){std::cerr<<"Compatible coherent fallback failed";return 1;}
 if(snapshotMemoryType(m,1,true)!=UINT32_MAX||snapshotMemoryType(m,0,false)!=UINT32_MAX){std::cerr<<"Unmappable memory accepted";return 1;}
 m.memoryTypes[3].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
 if(snapshotMemoryType(m,8,true)!=UINT32_MAX||snapshotMemoryType(m,15,true)!=1){std::cerr<<"Noncoherent memory accepted";return 1;}
 m.memoryTypeCount=0;if(snapshotMemoryType(m,15,true)!=UINT32_MAX)return 1;
 std::cout<<"Snapshot memory compatibility, coherency and CPU preservation passed\n";
}
