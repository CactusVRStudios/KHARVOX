#pragma once
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>
namespace kharvox::native {
// Small ordered CPU work lists. Same key order/duplicate semantics as map,
// but clear retains capacity. Insertion invalidates iterators: callers consume
// emplace's result immediately and never keep it across another insertion.
template<class K,class V,class Less=std::less<K>> class OrderedWorkMap {
    std::vector<std::pair<K,V>> values;
public:
    auto begin(){return values.begin();} auto end(){return values.end();}
    auto begin() const{return values.begin();} auto end() const{return values.end();}
    bool empty() const{return values.empty();}
    size_t size() const{return values.size();}
    void clear(){values.clear();}
    auto emplace(const K& key,const V& value){
        Less less;
        auto it=std::lower_bound(values.begin(),values.end(),key,
            [&](const auto& item,const K& needle){return less(item.first,needle);});
        if(it!=values.end()&&!less(key,it->first))return std::make_pair(it,false);
        return std::make_pair(values.insert(it,{key,value}),true);
    }
};
}
