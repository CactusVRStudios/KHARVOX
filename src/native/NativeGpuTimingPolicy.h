#pragma once
#include <cstdint>
#include <optional>
namespace kharvox::native {
constexpr uint32_t gpuTimingSampleLimit=96;
constexpr bool gpuTimingSampleDue(bool ready,uint64_t eligible,uint32_t samples){return ready&&eligible!=0&&eligible%120==0&&samples<gpuTimingSampleLimit;}
inline std::optional<double> timestampMilliseconds(uint64_t start,uint64_t end,bool startAvailable,bool endAvailable,uint32_t bits,double period){
 if(!startAvailable||!endAvailable||!bits||bits>64||!(period>0))return {};
 const uint64_t mask=bits==64?UINT64_MAX:(uint64_t(1)<<bits)-1;
 return double((end-start)&mask)*period/1e6;
}
}
