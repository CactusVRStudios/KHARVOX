#pragma once
#include "../hands/HandSceneDepthTracker.h"

namespace kharvox::native {
// Called under the owner's mirror lock. No allocation or tracker registration:
// these views are owned by Native and retained through its Present completion.
template<class Views,class Layouts,class Written>
bool resolveHandSceneMirror(const hands::HandSceneTarget& left,VkImage expectedColor,
    const Views& views,const Layouts& layouts,const Written& written,hands::HandSceneTarget& right){
    right={};
    const auto color=views.find(left.colorView),depth=views.find(left.depthView);
    if(color==views.end()||depth==views.end())return false;
    const auto& c=color->second;const auto& d=depth->second;
    const auto layout=layouts.find(d.image);
    if(!expectedColor||c.image!=expectedColor||c.image==left.colorImage||d.image==left.depthImage||!c.view||!d.view
        ||c.format!=left.colorFormat||d.format!=left.depthFormat
        ||c.extent.width!=left.extent.width||c.extent.height!=left.extent.height
        ||d.extent.width!=left.extent.width||d.extent.height!=left.extent.height
        ||layout==layouts.end()||layout->second==VK_IMAGE_LAYOUT_UNDEFINED
        ||!written.count(d.image))return false;
    right=left;right.colorImage=c.image;right.colorView=c.view;
    right.depthImage=d.image;right.depthView=d.view;right.depthLayout=layout->second;
    right.copyDepthForHands=true;
    return true;
}
}
