#pragma once
#include <mutex>

namespace kharvox {
// Copy only the requested function under the original registry lock. No
// cached device, generation, key or reference can outlive this lookup. The
// caller releases the lock before invoking downstream (which may re-enter).
template<class Registry,class Key,class Dispatch,class Member>
Member lookupDispatchMember(std::mutex& mutex,const Registry& registry,
                            const Key& key,Member Dispatch::* member){
    std::lock_guard<std::mutex> lock(mutex);
    const auto found=registry.find(key);
    return found==registry.end()?Member{}:found->second.*member;
}
// Nested dispatch tables obey the same lifetime and locking rules.
template<class Registry,class Key,class Dispatch,class Nested,class Member>
Member lookupDispatchMember(std::mutex& mutex,const Registry& registry,
                            const Key& key,Nested Dispatch::* group,Member Nested::* member){
    std::lock_guard<std::mutex> lock(mutex);
    const auto found=registry.find(key);
    return found==registry.end()?Member{}:(found->second.*group).*member;
}
}
