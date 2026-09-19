#pragma once

namespace kharvox {

class SwapchainImageState {
public:
    bool acquired(){if(phase!=Phase::Released)return false;phase=Phase::Acquired;return true;}
    bool waited(bool completed){if(!completed||phase!=Phase::Acquired)return false;phase=Phase::Waited;return true;}
    bool released(){if(phase!=Phase::Waited)return false;phase=Phase::Released;return true;}
    bool owned()const{return phase!=Phase::Released;}
    bool releasable()const{return phase==Phase::Waited;}
    void reset(){phase=Phase::Released;}
private:
    enum class Phase { Released, Acquired, Waited };
    Phase phase{Phase::Released};
};

}
