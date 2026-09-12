#pragma once
#include <cstdint>
#include <vector>
namespace kharvox::native {
// Compare full descriptor contents, not a hash. Dynamic offsets are passed
// separately to vkCmdBindDescriptorSets and intentionally are not in this key.
// The cache is scoped to one completed frame; no set outlives pool retirement.
using DescriptorReuseKey=std::vector<uint64_t>;
inline void descriptorKeyBuffer(DescriptorReuseKey& key,uint64_t binding,uint64_t type,uint64_t buffer,uint64_t offset,uint64_t range){
 key.insert(key.end(),{1,binding,type,buffer,offset,range});
}
inline void descriptorKeyImage(DescriptorReuseKey& key,uint64_t binding,uint64_t type,uint64_t view,uint64_t sampler,uint64_t layout){
 key.insert(key.end(),{2,binding,type,view,sampler,layout});
}
}
