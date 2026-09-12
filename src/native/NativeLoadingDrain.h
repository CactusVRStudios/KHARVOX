#pragma once
#include <cstdint>

namespace kharvox::native {
// A loading boundary may contain no injected commands at all. It still owns
// the memory-free transaction started by beginRoot. Keep that owner until the
// original root returns and Present verifies device completion; never publish
// a stereo identity for this path.
class EmptyLoadingDrain {
    bool active_{},rootReturned_{};
public:
    bool active() const { return active_; }
    bool begin(bool noRecordedOrSubmittedCommands,bool noStereoWork) {
        if(active_||!noRecordedOrSubmittedCommands||!noStereoWork)return false;
        active_=true;rootReturned_=false;return true;
    }
    bool rootReturned() {
        if(!active_||rootReturned_)return false;
        rootReturned_=true;return true;
    }
    bool recordingFinished(bool stillNoNativeWork) const {
        return active_&&rootReturned_&&stillNoNativeWork;
    }
    bool complete(bool stillNoNativeWork,bool gpuComplete) {
        if(!recordingFinished(stillNoNativeWork)||!gpuComplete)return false;
        active_=rootReturned_=false;return true;
    }
};
// A loading FrameRoot can return inside its original render pass. Keep the
// original-only transaction alive until the engine closes/submits it and the
// owner confirms GPU completion. This state can never authorize a stereo pair.
class LoadingDrain {
    uintptr_t command_{};
    bool rootReturned_{};
public:
    bool active() const { return command_ != 0; }
    uintptr_t command() const { return command_; }
    bool begin(uintptr_t command, bool knownRecordingPass,
               bool singleUnsubmittedOriginal, bool stereoWorkStarted) {
        if (active() || !command || !knownRecordingPass ||
            !singleUnsubmittedOriginal || stereoWorkStarted) return false;
        command_ = command;
        rootReturned_ = false;
        return true;
    }
    bool rootReturned() {
        if (!active() || rootReturned_) return false;
        rootReturned_ = true;
        return true;
    }
    bool recordingFinished(bool commandClosed) const {
        return active() && rootReturned_ && commandClosed;
    }
    bool complete(bool commandClosed, bool submitted, bool gpuComplete) {
        if (!recordingFinished(commandClosed) || !submitted || !gpuComplete) return false;
        command_ = 0;
        rootReturned_ = false;
        return true;
    }
};
}
