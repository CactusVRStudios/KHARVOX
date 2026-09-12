#pragma once
#include <cstdint>
namespace kharvox::native::cpu {
constexpr bool sparseDetailFrame(uint64_t serial,bool sparse,bool continuous){
    return sparse&&!continuous&&serial!=0&&serial%120==60;
}
constexpr bool detailFrame(uint64_t serial,bool sparse,bool continuous){
    return continuous||sparseDetailFrame(serial,sparse,continuous);
}
}
