#include "../src/native/NativeSpirvReadOnly.h"
#include <iostream>
using namespace kharvox::native;
static std::vector<uint32_t> module(bool allMembers,bool wholeVariable=false,bool modern=false){
 std::vector<uint32_t> w{0x07230203,0x10000,0,20,0};
 auto op=[&](uint32_t code,std::initializer_list<uint32_t> args){w.push_back(uint32_t(args.size()+1)<<16|code);w.insert(w.end(),args);};
 op(30,{1,10,10});op(32,{2,modern?12u:2u,1});op(59,{2,3,modern?12u:2u});
 if(!modern)op(71,{1,3});op(71,{3,34,0});op(71,{3,33,24});
 op(72,{1,0,24});if(allMembers)op(72,{1,1,24});if(wholeVariable)op(71,{3,24});return w;
}
int main(){int failures=0;auto check=[&](bool ok){if(!ok)++failures;};
 auto read=[&](const auto& w){return reflectStorageAccess(w.data(),w.size());};
 auto w=module(true);auto r=read(w);check(r.valid&&r.bindings.at({0,24}).readOnly);
 r=read(module(false));check(r.valid&&!r.bindings.at({0,24}).readOnly);
 r=read(module(false,true));check(r.valid&&r.bindings.at({0,24}).readOnly);
 r=read(module(true,false,true));check(r.valid&&r.bindings.at({0,24}).readOnly);
 // Descriptor-class binding alone never proves shader write access.
 w=module(false);w[5+1]=11;check(!read(w).bindings.at({0,24}).readOnly);
 w=module(true);w.push_back(0);check(!read(w).valid);
 w=module(true);w.pop_back();check(!read(w).valid);
 w=module(true);w.push_back(2u<<16|73);w.push_back(7);check(!read(w).valid);
 w=module(true);w[0]=0;check(!read(w).valid);
 check(!reflectStorageAccess(nullptr,0).valid);
 // A writable alias to the same binding must veto a read-only declaration.
 w=module(true,false,true);
 const uint32_t more[]={3u<<16|30,11,10,4u<<16|32,12,12,11,4u<<16|59,12,13,12,4u<<16|71,13,34,0,4u<<16|71,13,33,24};
 w.insert(w.end(),std::begin(more),std::end(more));r=read(w);check(r.valid&&!r.bindings.at({0,24}).readOnly);
 check(!permitsReadOnlySnapshot(r,{0,24}));
 check(permitsReadOnlySnapshot(r,{1,24})); // Unused binding is not consumed.
 r=read(module(true));check(permitsReadOnlySnapshot(r,{0,24}));
 r.valid=false;check(!permitsReadOnlySnapshot(r,{0,24}));
 std::cout<<"Native SPIR-V storage access failures="<<failures<<'\n';return failures?1:0;
}
