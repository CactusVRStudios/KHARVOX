#pragma once
namespace kharvox {
inline bool freshAerHandsEnabled(bool coherentPair,bool native,bool showHands,
    bool gameplay,bool cutscene,bool immersive) {
    return coherentPair&&!native&&showHands&&gameplay&&!cutscene&&!immersive;
}
inline bool freshAerHandsUpdateTargets(bool reusePair,bool freshHands,bool cleanPairReady) {
    return !reusePair||(freshHands&&cleanPairReady);
}
}
