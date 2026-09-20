#include <vulkan/vulkan.h>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>

namespace kharvox::native {
struct ReplayState {bool recording{};std::shared_ptr<int> payload;};
static std::map<VkCommandBuffer,ReplayState> replayStates;
static std::map<VkCommandBuffer,int> boundStorageSnapshots;
static std::set<VkCommandBuffer> rightEyeBindingCommands;
static std::recursive_mutex replayMutex;
static struct Cache {
 std::set<VkCommandBuffer> entries;
 void invalidate(VkCommandBuffer cb){entries.erase(cb);}
 void clear(){entries.clear();}
} snapshotBatchCache;
static void resetStorageSnapshotBindings(VkCommandBuffer cb){snapshotBatchCache.invalidate(cb);const auto it=boundStorageSnapshots.find(cb);if(it!=boundStorageSnapshots.end())it->second=0;}
static struct Drain {
 bool draining{};uintptr_t cb{};
 bool active()const{return draining;}
 uintptr_t command()const{return cb;}
} loadingDrain;
[[noreturn]] static void fail(const char* message){throw std::runtime_error(message);}
static VkResult status=VK_SUCCESS;
static bool outstanding{};
static void beforeDestroy(const char*,uintptr_t){if(outstanding)fail("outstanding");}
static uintptr_t nextHandle=1;
static unsigned destroys{},frees{};
static VkResult VKAPI_CALL hook_allocate_command_buffers(VkDevice,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* commands){
 if(status==VK_SUCCESS)for(uint32_t i=0;i<info->commandBufferCount;++i)commands[i]=reinterpret_cast<VkCommandBuffer>(nextHandle++);
 return status;
}
static void VKAPI_CALL hook_destroy_command_pool(VkDevice,VkCommandPool,const VkAllocationCallbacks*){if(outstanding)fail("outstanding");++destroys;}
static VkResult VKAPI_CALL begin(VkCommandBuffer,const VkCommandBufferBeginInfo*){return status;}
static VkResult VKAPI_CALL reset(VkCommandBuffer,VkCommandBufferResetFlags){return status;}
static VkResult VKAPI_CALL resetPool(VkDevice,VkCommandPool,VkCommandPoolResetFlags){return status;}
static void VKAPI_CALL freeBuffers(VkDevice,VkCommandPool,uint32_t,const VkCommandBuffer*){++frees;}
static PFN_vkBeginCommandBuffer nextBeginCommand=begin;
#include "../src/native/NativeCommandLifetime.inc"
}

int main(){
 using namespace kharvox::native;
 nextResetCommand=reset;nextResetCommandPool=resetPool;nextFreeCommands=freeBuffers;
 const auto pool=reinterpret_cast<VkCommandPool>(uintptr_t(1));
 const auto otherPool=reinterpret_cast<VkCommandPool>(uintptr_t(2));
 const auto allocate=[&](VkCommandPool owner){
  VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};info.commandPool=owner;info.commandBufferCount=1;
  VkCommandBuffer cb{};assert(replayAllocateCommands({},&info,&cb)==VK_SUCCESS);return cb;
 };
 const auto capture=[](VkCommandBuffer cb){
  replayStates[cb].payload=std::make_shared<int>(42);
  boundStorageSnapshots[cb]=1;rightEyeBindingCommands.insert(cb);snapshotBatchCache.entries.insert(cb);
  return std::weak_ptr<int>(replayStates[cb].payload);
 };
 const auto other=allocate(otherPool);const auto otherPayload=capture(other);
 for(unsigned i=0;i<1000;++i){
  const auto cb=allocate(pool);const auto payload=capture(cb);
  replayDestroyCommandPool({},pool,nullptr);
  assert(payload.expired()&&!otherPayload.expired());
  assert(commandPools.size()==1&&replayStates.size()==1&&boundStorageSnapshots.size()==1);
  assert(rightEyeBindingCommands==std::set<VkCommandBuffer>{other});
  assert(snapshotBatchCache.entries==std::set<VkCommandBuffer>{other});
 }
 const auto cb=allocate(pool);const auto payload=capture(cb);
 status=VK_ERROR_OUT_OF_HOST_MEMORY;
 assert(replayResetCommandPool({},pool,0)==status&&!payload.expired());
 assert(replayResetCommand(cb,0)==status&&!payload.expired());
 VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};info.commandPool=pool;info.commandBufferCount=1;
 VkCommandBuffer failed{};
 assert(replayAllocateCommands({},&info,&failed)==status&&commandPools.size()==2);
 status=VK_SUCCESS;
 assert(replayResetCommandPool({},pool,0)==VK_SUCCESS&&payload.expired());
 assert(commandPools.size()==2&&!otherPayload.expired());
 const auto fresh=capture(cb);
 assert(replayBeginCommand(cb,nullptr)==VK_SUCCESS&&fresh.expired());
 assert(commandPools.size()==2&&replayStates[cb].recording);
 const auto resetPayload=capture(cb);
 assert(replayResetCommand(cb,0)==VK_SUCCESS&&resetPayload.expired());
 const auto freed=capture(cb);
 replayFreeCommands({},pool,1,&cb);
 assert(freed.expired()&&frees==1&&commandPools.size()==1);
 nextHandle=reinterpret_cast<uintptr_t>(cb);
 assert(allocate(otherPool)==cb);
 const auto reused=capture(cb);
 replayDestroyCommandPool({},pool,nullptr);
 assert(!reused.expired()&&!otherPayload.expired());
 outstanding=true;bool rejected{};
 try{replayDestroyCommandPool({},otherPool,nullptr);}catch(const std::runtime_error&){rejected=true;}
 assert(rejected&&!reused.expired()&&!otherPayload.expired());
 outstanding=false;
 replayDestroyCommandPool({},otherPool,nullptr);
 assert(reused.expired()&&otherPayload.expired()&&commandPools.empty()&&replayStates.empty());
 assert(boundStorageSnapshots.empty()&&rightEyeBindingCommands.empty()&&snapshotBatchCache.entries.empty());
}
