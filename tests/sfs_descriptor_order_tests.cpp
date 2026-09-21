#include "../src/sfs/DescriptorBindOrder.h"
#include <array>
#include <cassert>
#ifdef _WIN32
#include "../src/sfs/NativeSfs.cpp"
namespace kharvox::sfs {
CompiledShader compileStereoShader(const std::vector<uint32_t>&,const ShaderCompileOptions&){throw std::runtime_error("Unexpected compilation");}
bool hasStereoStorageOutput(const std::vector<uint32_t>&){throw std::runtime_error("Unexpected inspection");}
bool needsHeadsetProjection(const std::vector<uint32_t>&,bool,bool){throw std::runtime_error("Unexpected projection");}
}

std::vector<uint32_t> replayed;
VkPipeline lastPipeline{};
void VKAPI_CALL recordBind(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t first,uint32_t count,const VkDescriptorSet*,uint32_t dynamicCount,const uint32_t* dynamic) {
    assert(count == 1 && dynamicCount == 1 && dynamic[0] == first + 10);
    replayed.push_back(first);
}
void VKAPI_CALL recordPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline pipeline) { lastPipeline = pipeline; }
void VKAPI_CALL recordPass(VkCommandBuffer,const VkRenderPassBeginInfo*,VkSubpassContents) {}
void VKAPI_CALL recordNext(VkCommandBuffer,VkSubpassContents) {}
VkResult VKAPI_CALL recordBegin(VkCommandBuffer,const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
static unsigned barrierCalls{};
void VKAPI_CALL recordBufferBarrier(VkCommandBuffer,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags deps,uint32_t nm,const VkMemoryBarrier* m,uint32_t nb,const VkBufferMemoryBarrier* b,uint32_t ni,const VkImageMemoryBarrier*) {
    assert(src==VK_PIPELINE_STAGE_TRANSFER_BIT&&dst==VK_PIPELINE_STAGE_VERTEX_INPUT_BIT&&deps==0);
    assert(nm==1&&m&&m->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT);
    assert(nb==1&&b&&b->offset==32&&b->size==64&&ni==0);++barrierCalls;
}
void testRuntime() {
    using namespace kharvox::sfs;
    void* dispatch = reinterpret_cast<void*>(uintptr_t(100));
    const auto cb = reinterpret_cast<VkCommandBuffer>(&dispatch);
    auto state = std::make_shared<State>();
    devices[dispatch] = state;
    state->dispatch.vkCmdPipelineBarrier=recordBufferBarrier;
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};memory.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    VkBufferMemoryBarrier buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};buffer.offset=32;buffer.size=64;
    // No command registry entry exists, and the metadata lock is unavailable.
    // A buffer-only barrier must still reach the device unchanged.
    {
        std::unique_lock held(state->mutex);
        barriers(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,0,1,&memory,1,&buffer,0,nullptr);
    }
    assert(barrierCalls==1);
    state->dispatch.vkCmdBindDescriptorSets = recordBind;
    state->dispatch.vkCmdBindPipeline = recordPipeline;
    state->dispatch.vkCmdBeginRenderPass = recordPass;
    state->dispatch.vkCmdNextSubpass = recordNext;
    state->dispatch.vkBeginCommandBuffer = recordBegin;
    auto& command = state->commands[cb];
    command.graphics = reinterpret_cast<VkPipeline>(uintptr_t(3));
    const auto stereoPipeline = reinterpret_cast<VkPipeline>(uintptr_t(4));
    state->stereoPipelines[command.graphics] = stereoPipeline;
    for (auto index : {1u, 0u}) {
        const auto set = reinterpret_cast<VkDescriptorSet>(uintptr_t(index + 1));
        const auto layout = reinterpret_cast<VkPipelineLayout>(uintptr_t(index + 1));
        state->setDynamicCounts[set] = 1;
        const auto dynamic = index + 10;
        bindSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,index,1,&set,1,&dynamic);
    }
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = reinterpret_cast<VkRenderPass>(uintptr_t(5));
    pass.framebuffer = reinterpret_cast<VkFramebuffer>(uintptr_t(6));
    state->passes[pass.renderPass] = reinterpret_cast<VkRenderPass>(uintptr_t(7));
    state->mixedFramebuffers[pass.framebuffer] = false;
    state->framebufferStereo[pass.framebuffer] = true;
    replayed.clear();
    beginPass(cb,&pass,VK_SUBPASS_CONTENTS_INLINE);
    assert((replayed == std::vector<uint32_t>{1,0}) && lastPipeline == stereoPipeline);
    replayed.clear(); nextPass(cb,VK_SUBPASS_CONTENTS_INLINE);
    assert((replayed == std::vector<uint32_t>{1,0}));
    state->framebufferStereo[pass.framebuffer] = false;
    replayed.clear(); beginPass(cb,&pass,VK_SUBPASS_CONTENTS_INLINE);
    assert(replayed.empty() && lastPipeline == command.graphics);
    nextPass(cb,VK_SUBPASS_CONTENTS_INLINE); assert(replayed.empty());
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(beginCommand(cb,&begin) == VK_SUCCESS);
    state->framebufferStereo[pass.framebuffer] = true;
    beginPass(cb,&pass,VK_SUBPASS_CONTENTS_INLINE); assert(replayed.empty());
    devices.erase(dispatch);
}
#endif

namespace {
constexpr std::array<std::array<unsigned, 5>, 5> layouts{{
    {0, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {1, 0, 0, 0, 0},
    {0, 0, 1, 0, 0}, {0, 0, 0, 0, 1}
}};
struct Binding { int layout{-1}; unsigned value{}; };
using Bindings = std::array<Binding, 4>;
bool compatible(int a, int b, unsigned index) {
    if (a < 0 || b < 0 || layouts[a][4] != layouts[b][4]) return false;
    for (unsigned i = 0; i <= index; ++i) if (layouts[a][i] != layouts[b][i]) return false;
    return true;
}
void bindModel(Bindings& bindings, unsigned index, Binding value) {
    for (unsigned i = 0; i < index; ++i)
        if (bindings[i].layout >= 0 && !compatible(bindings[i].layout, value.layout, i))
            bindings[i] = {value.layout, 0};
    if (bindings[index].layout >= 0 && !compatible(bindings[index].layout, value.layout, index))
        for (unsigned i = index + 1; i < bindings.size(); ++i)
            if (bindings[i].layout >= 0) bindings[i] = {value.layout, 0};
    bindings[index] = value;
}
void checkDefined(const Bindings& expected, const Bindings& actual) {
    for (unsigned i = 0; i < expected.size(); ++i)
        if (expected[i].value) {
            assert(expected[i].value == actual[i].value);
            assert(compatible(expected[i].layout, actual[i].layout, i));
        }
}
}

int main() {
#ifdef _WIN32
    testRuntime();
#endif
    kharvox::sfs::DescriptorBindOrder order;
    Bindings cached{}, expected{};
    cached[1] = {0, 1}; bindModel(expected, 1, cached[1]); order.record(1, 1);
    cached[0] = {2, 2}; bindModel(expected, 0, cached[0]); order.record(0, 1);
    assert((order.indices() == std::vector<uint32_t>{1, 0}));
    auto broken = expected;
    bindModel(broken, 0, cached[0]); bindModel(broken, 1, cached[1]);
    assert(expected[0].value == 2 && broken[0].value == 0);
    Bindings restored{};
    for (auto index : order.indices()) bindModel(restored, index, cached[index]);
    checkDefined(expected, restored);

    uint32_t random = 1337;
    const auto next = [&] { random = random * 1664525u + 1013904223u; return random >> 16; };
    for (unsigned trial = 0; trial < 10000; ++trial) {
        order.clear(); cached = {}; expected = {}; restored = {};
        for (unsigned step = 1; step <= 100; ++step) {
            const auto first = next() % 4;
            const auto count = next() % (5 - first);
            const auto layout = int(next() % layouts.size());
            order.record(first, count);
            for (unsigned index = first; index < first + count; ++index) {
                cached[index] = {layout, step}; bindModel(expected, index, cached[index]);
                bindModel(restored, index, cached[index]);
            }
            checkDefined(expected, restored);
            assert(order.indices().size() <= 4);
            restored = {};
            for (auto index : order.indices()) {
                bindModel(restored, index, cached[index]);
            }
            checkDefined(expected, restored);
        }
    }
    order.clear(); assert(order.indices().empty());
    for (unsigned repeat = 0; repeat < 10000; ++repeat) order.record(1, 2);
    assert((order.indices() == std::vector<uint32_t>{1, 2}));
}
