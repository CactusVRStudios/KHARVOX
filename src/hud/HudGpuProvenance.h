#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kharvox::hudtrace {
struct Source {
    std::uint64_t event{},gui{},generation{},level{},present{};
    int panel{-1};
    explicit operator bool()const noexcept{return event&&gui&&generation&&panel>=0&&panel<3;}
};
struct Finding {
    std::uint64_t commandBuffer{},recording{},ordinal{},submission{};
    Source source;
    std::string status,details;
    // Exact allocation lineage is distinct from safe future native replay.
    bool exclusiveObservedRanges{},replaySafe{};
};

// No shader fingerprints, timestamps or draw-shape similarity authorize an
// attribution. Only the actual fetched vertex byte ranges do. Unsupported
// uploads, indirect arguments, partial/mixed ownership and stale recordings
// remain explicitly unresolved. The tracker never executes GPU commands.
class Tracker {
public:
    Tracker();~Tracker();
    Tracker(const Tracker&)=delete;Tracker& operator=(const Tracker&)=delete;
    void ownGui(std::uint64_t gui,int panel,std::uint64_t present,std::uint64_t level);
    Source beginGui(std::uint64_t gui,std::uint64_t present,std::uint64_t level);
    void endGui(const Source&);
    void allocation(const Source&,std::uint64_t address,std::uint64_t bytes,
        std::uint64_t material=0,std::uint64_t inputIndices=0,unsigned indexCount=0);
    void allocateMemory(VkDevice,VkDeviceMemory,VkDeviceSize);
    void freeMemory(VkDevice,VkDeviceMemory);
    void mapMemory(VkDevice,VkDeviceMemory,VkDeviceSize,VkDeviceSize,void*);
    void unmapMemory(VkDevice,VkDeviceMemory);
    void createBuffer(VkDevice,VkBuffer,const VkBufferCreateInfo&);
    void destroyBuffer(VkDevice,VkBuffer);
    void bindBufferMemory(VkDevice,VkBuffer,VkDeviceMemory,VkDeviceSize);
    void allocateCommands(VkDevice,VkCommandPool,VkCommandBufferLevel,unsigned,const VkCommandBuffer*);
    void freeCommands(unsigned,const VkCommandBuffer*);
    void resetPool(VkDevice,VkCommandPool);
    void begin(VkCommandBuffer,const VkCommandBufferBeginInfo&);
    void end(VkCommandBuffer,bool success);
    void reset(VkCommandBuffer);
    void execute(VkCommandBuffer,unsigned,const VkCommandBuffer*);
    void createPipelines(VkDevice,unsigned,const VkGraphicsPipelineCreateInfo*,const VkPipeline*);
    void destroyPipeline(VkDevice,VkPipeline);
    void descriptors(VkDevice,unsigned,const VkWriteDescriptorSet*,unsigned,const VkCopyDescriptorSet*);
    void bindDescriptors(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,unsigned,unsigned,
        const VkDescriptorSet*,unsigned,const std::uint32_t*);
    void bindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline);
    void bindVertices(VkCommandBuffer,unsigned,unsigned,const VkBuffer*,const VkDeviceSize*);
    void bindIndex(VkCommandBuffer,VkBuffer,VkDeviceSize,VkIndexType);
    void push(VkCommandBuffer,VkPipelineLayout,VkShaderStageFlags,unsigned,unsigned,const void*);
    void viewport(VkCommandBuffer,unsigned,unsigned,const VkViewport*);
    void scissor(VkCommandBuffer,unsigned,unsigned,const VkRect2D*);
    void beginPass(VkCommandBuffer,const VkRenderPassBeginInfo&,VkSubpassContents);
    void nextPass(VkCommandBuffer,VkSubpassContents);
    void endPass(VkCommandBuffer);
    void draw(VkCommandBuffer,unsigned count,unsigned instances,unsigned first,int vertexOffset,
        unsigned firstInstance,bool indexed);
    void indirect(VkCommandBuffer,VkBuffer,VkDeviceSize,unsigned,unsigned,bool indexed);
    void copy(VkCommandBuffer,VkBuffer,VkBuffer,unsigned,const VkBufferCopy*);
    void update(VkCommandBuffer,VkBuffer,VkDeviceSize,VkDeviceSize,const void*);
    void barrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,
        unsigned,const VkMemoryBarrier*,unsigned,const VkBufferMemoryBarrier*,unsigned,const VkImageMemoryBarrier*);
    void submit(VkQueue,unsigned,const VkSubmitInfo*,VkFence,bool success);
    void submit2(VkQueue,unsigned,const VkSubmitInfo2*,VkFence,bool success);
    std::vector<Finding> findings()const;
    std::string report()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};

// Opt-in only: KHARVOX_HUD_PROVENANCE=1. Disabled path does not collect state
// or enable the old statistical HUD9 capture.
Tracker* active() noexcept;
void flushReport() noexcept;
class GuiScope {
    Tracker* tracker_{};Source previous_{},source_{};
public:
    GuiScope(void* gui,std::uint64_t present,std::uint64_t level,bool root);
    ~GuiScope();
};
void nativeAllocation(void* gui,void* vertices,unsigned count,void* indices,unsigned indexCount,void* material);
} // namespace kharvox::hudtrace

// Observational callbacks must never turn a diagnostic failure into a game
// failure. No downstream Vulkan/native call is executed inside this guard.
#define KHARVOX_HUD_TRACE(call) do { try { if(auto* hudTrace=kharvox::hudtrace::active()) { hudTrace->call; } } catch(...) {} } while(false)
