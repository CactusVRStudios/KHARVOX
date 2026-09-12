#pragma once
#include <cstdint>
namespace kharvox::native {
class FrameTracePolicy {
 bool held{},armed{},running{};uint32_t frames{},captures{};
public:
 static constexpr uint32_t frameLimit=4,captureLimit=8;
 void key(bool down){if(down&&!held&&!running&&captures<captureLimit)armed=true;held=down;}
 bool startBoundary(bool ready){if(!armed||!ready||running)return false;armed=false;running=true;frames=0;++captures;return true;}
 bool finishBoundary(bool ready){if(!running)return false;if(!ready||++frames>=frameLimit){running=false;return true;}return false;}
 void stop(){running=false;armed=false;}
 bool isRunning()const{return running;}
 uint32_t capture()const{return captures;}
 uint32_t completed()const{return frames;}
};
}
