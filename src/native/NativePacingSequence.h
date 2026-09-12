#pragma once
#include <array>
#include <cstddef>

namespace kharvox::native::cpu {
// One bounded capture. No allocation, clock read, logging or engine access.
// Warmup requires consecutive ready frames; gaps inside a capture remain in
// the caller's serial/timestamp fields and must not be treated as adjacent.
template<class Record, size_t Capacity, size_t Warmup>
class PacingSequence {
    static_assert(Capacity > 0);
    std::array<Record, Capacity> records_{};
    size_t count_{}, warmup_{};
    bool done_{};
public:
    bool push(const Record& record, bool ready) {
        if(done_)return false;
        if(!ready){warmup_=0;return seal();}
        if(warmup_<Warmup){++warmup_;return false;}
        records_[count_++]=record;
        if(count_==Capacity){done_=true;return true;}
        return false;
    }
    bool seal(){if(done_||!count_)return false;done_=true;return true;}
    size_t size()const{return count_;}
    const Record& operator[](size_t index)const{return records_[index];}
};
}
