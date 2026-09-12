#pragma once
#include <vector>
#include <cstring>
namespace kharvox::native {
// This broader causal comparison is deliberately distinct from sharing only
// shadow addressing. View-dependent cluster lists are included, so it is not
// production stereo lighting: edge-of-frustum light coverage needs acceptance.
inline bool sharedLightingSnapshotEligible(size_t bytes,bool uniform,bool readOnlyStorage){
 return uniform ? (bytes==65520||bytes==65536||bytes==49152||bytes==1024)
                : readOnlyStorage&&(bytes==24576||bytes==3145728);
}
// A diagnostic may pair left atlas texels with left addressing only if the
// right pass still describes the same world-space shadows and light indices.
inline bool sharedShadowTableCompatible(const std::vector<unsigned char>& left,
                                       const std::vector<unsigned char>& right){
 if(left.size()!=65520||right.size()!=left.size())return false;
 for(size_t offset=0;offset<left.size();offset+=80)
  if(std::memcmp(left.data()+offset,right.data()+offset,64))return false;
 return true; // the final float4 is atlas scale/bias, deliberately paired left
}
}
