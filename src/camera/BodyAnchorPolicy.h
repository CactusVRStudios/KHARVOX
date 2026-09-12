#pragma once
namespace kharvox {
inline bool mayCalibrateBodyAnchor(bool gameplayCamera,bool animated,bool weaponControl,bool bossSequence){
    return gameplayCamera&&!animated&&weaponControl&&!bossSequence;
}
}
