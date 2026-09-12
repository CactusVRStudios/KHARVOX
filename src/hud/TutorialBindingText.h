#pragma once
#include <string>
#include <string_view>

namespace kharvox {
// Physical controls used by createGameplayActions/updateVirtualButtons.
// Keep action matching exact: never replace part of another identifier.
inline std::string tutorialBindingLabel(std::string_view action,bool left,bool swapSticks){
    const bool full=left&&swapSticks;
    const std::string weapon=left?"L ":"R ";
    const std::string support=left?"R ":"L ";
    const std::string turn=full?"L ":"R ";
    if(action=="_attack2"||action=="_use")return weapon+"Stick click";
    if(action=="_attack1")return weapon+"Trigger";
    if(action=="_zoom")return weapon+"Grip";
    if(action=="_quick3")return turn+"Stick up";
    if(action=="_changeWeapon")return turn+"Stick down: tap / hold";
    if(action=="_quickuse")return support+"Trigger";
    if(action=="_quick2"||action=="_quick0")return support+"Grip tap (outside grab range)";
    if(action=="_supermeter")return support+"Grip hold (without two-hand support)";
    if(action=="_jump")return full?"Y":"B";
    if(action=="_crouch")return full?"X":"A";
    if(action=="_inventory"||action=="_objectives")return full?"A":"X";
    return {};
}
inline std::string replaceTutorialBindings(std::string_view text,bool left,bool swapSticks){
    std::string result;result.reserve(text.size());
    auto identifier=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';};
    for(size_t i=0;i<text.size();){
        if(text[i]=='_'&&(i==0||!identifier(text[i-1])||(i>=2&&text[i-2]=='^'))){
            size_t end=i+1;while(end<text.size()&&identifier(text[end]))++end;
            const auto label=tutorialBindingLabel(text.substr(i,end-i),left,swapSticks);
            if(!label.empty()){result+='[';result+=label;result+=']';i=end;continue;}
        }
        result+=text[i++];
    }
    return result;
}
}
