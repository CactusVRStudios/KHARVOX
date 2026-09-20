#pragma once

namespace kharvox {

class CommandRecordingState {
public:
    bool reset(bool succeeded){phase=succeeded?Phase::Reset:Phase::Failed;return succeeded;}
    bool begun(bool succeeded){phase=phase==Phase::Reset&&succeeded?Phase::Begun:Phase::Failed;return phase==Phase::Begun;}
    bool ended(bool succeeded){phase=phase==Phase::Begun&&succeeded?Phase::Executable:Phase::Failed;return phase==Phase::Executable;}
    bool executable()const{return phase==Phase::Executable;}
private:
    enum class Phase { Initial, Reset, Begun, Executable, Failed };
    Phase phase{Phase::Initial};
};

}
