#include "../src/camera/AerCameraPairCache.h"
#include <thread>
#include <cstdlib>

void require(bool ok){if(!ok)std::abort();}
int main(){
    using namespace kharvox;
    AerCameraPairCache cache;
    constexpr uintptr_t game=0xE3F96A,stable=game^(uintptr_t{1}<<63);
    auto sample=[&](uintptr_t key,float x,bool expectHeld,float expected){
        float origin[3]{x,2,3},axis[9]{1,0,0,0,1,0,0,0,1};
        require(cache.resolve(key,origin,axis)==expectHeld);
        require(origin[0]==expected&&axis[0]==1&&axis[4]==1&&axis[8]==1);
    };
    // A worker seeing the second phase first must not seed a cache backwards.
    cache.begin(aerSecondRenderEye,true);
    sample(game,90,false,90);
    for(int pair=1;pair<=3;++pair){
        cache.begin(aerFirstRenderEye,true);
        std::thread producer([&]{sample(game,float(pair),false,float(pair));
            sample(stable,float(pair+10),false,float(pair+10));});
        producer.join();
        // Repeated first-phase work replaces only this pair's own snapshot.
        sample(game,float(pair+20),false,float(pair+20));
        cache.begin(aerSecondRenderEye,true);
        std::thread consumer([&]{sample(game,90,true,float(pair+20));
            sample(stable,90,true,float(pair+10));});
        consumer.join();
        sample(0xA30CFF,90,false,90); // Never borrow another camera's pose.
    }
    cache.begin(aerFirstRenderEye,true);
    // A first-phase omission cannot reuse the previous pair.
    cache.begin(aerSecondRenderEye,true);
    sample(game,90,false,90);
    cache.begin(aerFirstRenderEye,true);
    sample(game,15,false,15);
    cache.begin(aerSecondRenderEye,false);
    sample(game,90,false,90);
    cache.begin(aerSecondRenderEye,true);
    sample(game,90,false,90); // Mode change invalidates incomplete histories.
    // SFS uses only phase zero: walking must capture each new body position,
    // never freeze it for an alternating second CPU eye.
    for(int frame=0;frame<4;++frame){
        cache.begin(aerFirstRenderEye,true);
        sample(game,100.f+frame,false,100.f+frame);
    }
}
