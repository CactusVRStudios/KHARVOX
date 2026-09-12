#include "../src/native/NativeSnapshotAccessClasses.h"
#include "../src/native/NativeBindingMemo.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <random>

using namespace kharvox::native;
static void require(bool ok){if(!ok)std::abort();}
static bool eligible(const ShaderStorageAccess& access,uint32_t set,uint32_t binding){
 const auto it=access.bindings.find({set,binding});
 return access.valid&&it!=access.bindings.end()&&it->second.readOnly;
}
int main(){
 SnapshotAccessClasses classes;
 ShaderStorageAccess a{true,{{{0,4},{true,"a"}},{{0,7},{false,"write"}}}};
 auto b=a;b.bindings[{0,4}].name="unrelated pipeline";b.bindings[{2,99}]={true,"other set"};
 auto ca=classes.classify(a),cb=classes.classify(b);
 require(ca.forSet(0)==cb.forSet(0)&&ca.forSet(2)!=cb.forSet(2));
 auto writable=a;writable.bindings[{0,4}].readOnly=false;
 require(classes.classify(writable).forSet(0)!=ca.forSet(0));
 auto absent=a;absent.bindings.erase({0,4});
 require(classes.classify(absent).forSet(0)==classes.classify(writable).forSet(0));
 auto invalid=a;invalid.valid=false;require(!classes.classify(invalid).forSet(0));
 require(!SnapshotAccessClasses::Pipeline{}.forSet(0)); // missing pipeline
 // Class interning never reassigns an old signature after handle reuse.
 auto recycled=classes.classify(writable);require(recycled.forSet(0)!=ca.forSet(0));
 require(classes.classify(a).forSet(0)==ca.forSet(0));
 // Differential proof across random declarations: equal classes must make
 // the same snapshot choice for every candidate storage binding in this set.
 std::mt19937 rng(151);std::map<uint64_t,std::vector<bool>> decisions;
 for(int p=0;p<10000;++p){ShaderStorageAccess access;access.valid=true;
  for(uint32_t binding=0;binding<12;++binding){auto mode=rng()%3;
   if(mode)access.bindings[{0,binding}]={mode==2,"ignored"};}
  const auto id=*classes.classify(access).forSet(0);std::vector<bool> choice;
  for(uint32_t binding=0;binding<12;++binding)choice.push_back(eligible(access,0,binding));
  const auto [it,added]=decisions.emplace(id,choice);require(added||it->second==choice);
 }
 // Positive and pass-through results are scoped to the full original key
 // and mutation generation; identical access in a second pipeline can reuse.
 using Key=std::array<uint64_t,8>;struct Result{int descriptor;std::vector<uint32_t> storage;};
 BindingMemo<Key,Result> memo;BindingMutationClock clock;
 Key key{11,22,*ca.forSet(0),44,0,0,0,77};auto before=clock.stamp();
 memo.put(key,{88,{4}},before,clock.stamp());auto equivalent=key;equivalent[2]=*cb.forSet(0);
 require(memo.find(equivalent,clock.stamp())->descriptor==88);
 auto changed=key;changed[2]=*recycled.forSet(0);require(!memo.find(changed,clock.stamp()));
 for(size_t field:{0u,1u,3u,4u,5u,6u,7u}){changed=key;++changed[field];require(!memo.find(changed,clock.stamp()));}
 // A descriptor previously needing no clone can become a mapped UBO/storage
 // descriptor: a content/map mutation must force a fresh replacement plan.
 Key passThrough{33,33,1,44,0,0,0,77};
 memo.put(passThrough,{33,{}},before,before);require(memo.find(passThrough,before)->storage.empty());
 {BindingMutationScope update(clock);require(!memo.find(passThrough,clock.stamp()));}
 require(!memo.find(passThrough,clock.stamp()));
 // Mutation while classifying cannot publish a result with the old stamp.
 memo.put(key,{99,{4}},before,clock.stamp());require(!memo.find(key,clock.stamp()));
 before=clock.stamp();memo.put(key,{101,{4}},before,before);
 memo.clear();require(!memo.find(key,before));
 // Existing draw-time writable checks remain independent of memo classes.
 require(permitsReadOnlySnapshot(a,{0,4}));require(!permitsReadOnlySnapshot(writable,{0,4}));
 require(!permitsReadOnlySnapshot(invalid,{0,4}));
 std::cout<<"Snapshot access classes: 10000 differential pipelines; equivalent/changed/invalid access, negative-result mutation, full key separation and frame retirement passed\n";
}
