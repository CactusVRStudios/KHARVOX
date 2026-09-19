#pragma once

namespace kharvox {

class SessionReadiness {
public:
    void created(){exists=true;ready=false;}
    void completed(){if(exists)ready=true;}
    void restartRequired(){ready=false;}
    void reset(){exists=false;ready=false;}
    bool usable()const{return exists&&ready;}
private:
    bool exists{};
    bool ready{};
};

}
