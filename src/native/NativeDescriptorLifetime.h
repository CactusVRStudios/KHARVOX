#pragma once
#include <map>
#include <set>
#include <vector>
#include <cstdint>
namespace kharvox::native {
// Caller serializes access. Allocation recycles handles, free removes one set,
// and successful pool reset/destruction removes every member of that pool.
class DescriptorPoolLedger {
 std::map<uint64_t,uint64_t> owner;
 std::map<uint64_t,std::set<uint64_t>> members;
public:
 void allocated(uint64_t pool,uint64_t set){freed(set);owner[set]=pool;members[pool].insert(set);}
 void freed(uint64_t set){auto it=owner.find(set);if(it==owner.end())return;auto pool=members.find(it->second);if(pool!=members.end()){pool->second.erase(set);if(pool->second.empty())members.erase(pool);}owner.erase(it);}
 std::vector<uint64_t> reset(uint64_t pool){std::vector<uint64_t> result;auto it=members.find(pool);if(it==members.end())return result;result.assign(it->second.begin(),it->second.end());for(auto set:result)owner.erase(set);members.erase(it);return result;}
 size_t size()const{return owner.size();}
};
}
