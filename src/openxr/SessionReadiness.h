#pragma once

namespace kharvox {

class SessionReadiness {
public:
    void created(){exists=true;ready=false;}
    void completed(){if(exists)ready=true;}
    void restartRequired(){ready=false;}
    void exitRequested(){exiting=true;ready=false;}
    void systemRecoveryRequired(){systemLost=true;ready=false;}
    void reset(){exists=false;ready=false;}
    bool usable()const{return exists&&ready&&!exiting&&!systemLost;}
    bool canCreate()const{return !exiting&&!systemLost;}
private:
    bool exists{};
    bool ready{};
    bool exiting{};
    bool systemLost{};
};

}
