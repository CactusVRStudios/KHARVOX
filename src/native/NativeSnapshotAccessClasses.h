#pragma once
#include "NativeSpirvReadOnly.h"
#include <optional>

namespace kharvox::native {
// Immutable classification of the *snapshot decision*, not shader identity.
// A mapped storage binding is snapshotted only when explicitly NonWritable.
// Missing/writable bindings both stay on the original path. Names, other sets
// and shader handles do not change that decision. Invalid reflection never
// gets a class, so it must run the existing full validation path.
class SnapshotAccessClasses {
    std::map<std::vector<uint32_t>,uint64_t> signatures;
    uint64_t next{2};
public:
    struct Pipeline {
        bool valid{};
        std::map<uint32_t,uint64_t> sets;
        std::optional<uint64_t> forSet(uint32_t set) const {
            if(!valid)return {};
            const auto it=sets.find(set);
            return it==sets.end()?uint64_t(1):it->second; // no eligible binding
        }
    };
    Pipeline classify(const ShaderStorageAccess& access) {
        Pipeline result;result.valid=access.valid;if(!access.valid)return result;
        std::map<uint32_t,std::vector<uint32_t>> eligible;
        for(const auto& [key,value]:access.bindings)
            if(value.readOnly)eligible[key.first].push_back(key.second);
        for(const auto& [set,bindings]:eligible){
            auto [it,added]=signatures.emplace(bindings,next);
            if(added)++next;
            result.sets.emplace(set,it->second);
        }
        return result;
    }
};
}
