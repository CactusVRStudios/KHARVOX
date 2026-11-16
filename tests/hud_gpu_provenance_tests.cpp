#include "../src/hud/HudGpuProvenance.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>

using namespace kharvox::hudtrace;
namespace {
template<class T> T handle(std::uint64_t value){if constexpr(std::is_pointer_v<T>)return reinterpret_cast<T>(value);else return static_cast<T>(value);}
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Fixture {
    Tracker tracker;
    VkDevice device=handle<VkDevice>(1);
    VkBuffer vertices=handle<VkBuffer>(2),indices=handle<VkBuffer>(3),copied=handle<VkBuffer>(4);
    VkDeviceMemory vertexMemory=handle<VkDeviceMemory>(5),indexMemory=handle<VkDeviceMemory>(6);
    VkPipeline pipeline=handle<VkPipeline>(7);
    VkCommandBuffer primary=handle<VkCommandBuffer>(8),secondary=handle<VkCommandBuffer>(9);
    VkCommandPool pool=handle<VkCommandPool>(10);
    std::array<unsigned char,48*8> storage{};
    std::array<std::uint16_t,6> indexData{{0,1,2,2,3,0}};
    Source source;
    Fixture(int panel=0){
        VkBufferCreateInfo vb{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};vb.size=storage.size();vb.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        tracker.createBuffer(device,vertices,vb);tracker.allocateMemory(device,vertexMemory,storage.size());
        tracker.bindBufferMemory(device,vertices,vertexMemory,0);tracker.mapMemory(device,vertexMemory,0,VK_WHOLE_SIZE,storage.data());
        vb.size=sizeof(indexData);vb.usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        tracker.createBuffer(device,indices,vb);tracker.allocateMemory(device,indexMemory,sizeof(indexData));
        tracker.bindBufferMemory(device,indices,indexMemory,0);tracker.mapMemory(device,indexMemory,0,VK_WHOLE_SIZE,indexData.data());
        tracker.allocateCommands(device,pool,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1,&primary);
        tracker.allocateCommands(device,pool,VK_COMMAND_BUFFER_LEVEL_SECONDARY,1,&secondary);
        VkVertexInputBindingDescription binding{0,48,VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attribute{0,0,VK_FORMAT_R32G32B32_SFLOAT,0};
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertex.vertexBindingDescriptionCount=1;vertex.pVertexBindingDescriptions=&binding;vertex.vertexAttributeDescriptionCount=1;vertex.pVertexAttributeDescriptions=&attribute;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.pVertexInputState=&vertex;
        tracker.createPipelines(device,1,&info,&pipeline);
        tracker.ownGui(100,panel,30,1);source=tracker.beginGui(100,30,1);
        tracker.allocation(source,reinterpret_cast<std::uintptr_t>(storage.data()),storage.size(),77,0,indexData.size());tracker.endGui(source);
    }
    void begin(VkCommandBuffer cb,VkBuffer vertexBuffer=VK_NULL_HANDLE){
        VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};inheritance.renderPass=handle<VkRenderPass>(200);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};if(cb==secondary){begin.flags=VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;begin.pInheritanceInfo=&inheritance;}
        tracker.begin(cb,begin);tracker.bindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        auto buffer=vertexBuffer?vertexBuffer:vertices;VkDeviceSize offset{};tracker.bindVertices(cb,0,1,&buffer,&offset);
        tracker.bindIndex(cb,indices,0,VK_INDEX_TYPE_UINT16);
        const VkDescriptorSet set=handle<VkDescriptorSet>(300);const unsigned dynamicOffset=64;
        tracker.bindDescriptors(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,handle<VkPipelineLayout>(301),0,1,&set,1,&dynamicOffset);
        VkDescriptorImageInfo image{handle<VkSampler>(302),handle<VkImageView>(303),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;
        tracker.descriptors(device,1,&write,0,nullptr);
        const std::array<unsigned char,4> push{{0x12,0x34,0x56,0x78}};
        tracker.push(cb,handle<VkPipelineLayout>(301),VK_SHADER_STAGE_VERTEX_BIT,4,push.size(),push.data());
        VkViewport viewport{0,0,256,256,0,1};VkRect2D scissor{{0,0},{256,256}};
        tracker.viewport(cb,0,1,&viewport);tracker.scissor(cb,0,1,&scissor);
    }
    void draw(VkCommandBuffer cb){tracker.draw(cb,indexData.size(),1,0,0,0,true);}
    void submit(){VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&primary;
        tracker.submit(handle<VkQueue>(50),1,&info,handle<VkFence>(51),true);}
    Finding last(){auto values=tracker.findings();check(!values.empty(),"Missing finding");return values.back();}
    void simple(){begin(primary);draw(primary);tracker.end(primary,true);submit();}
};
}

int main(int argc,char**){try {
    unsigned cases{};
    for(int panel=0;panel<3;++panel){Fixture f(panel);f.simple();auto result=f.last();
        check(result.exclusiveObservedRanges&&result.source.event==f.source.event&&result.source.panel==panel,"Exact GUI range attribution failed");
        check(!result.replaySafe,"Geometry provenance must not authorize replay");
        const auto report=f.tracker.report();check(report.find("12345678")!=std::string::npos&&report.find("dynamicOffsets")!=std::string::npos
            &&report.find("descriptorContentsAtSubmission")!=std::string::npos,"Draw state capture incomplete");++cases;}
    {
        Fixture f;f.begin(f.primary);f.tracker.end(f.primary,true);
        std::thread worker([&]{f.begin(f.secondary);f.draw(f.secondary);f.tracker.end(f.secondary,true);});worker.join();
        f.begin(f.primary);f.tracker.execute(f.primary,1,&f.secondary);f.tracker.end(f.primary,true);f.submit();
        check(f.last().exclusiveObservedRanges&&f.last().commandBuffer==9,"Cross-thread secondary lost explicit ownership");
        check(f.last().details.find("executeOrdinal")!=std::string::npos,"Execution ancestry missing");++cases;
    }
    {
        Fixture f;f.begin(f.secondary);f.draw(f.secondary);f.tracker.end(f.secondary,true);
        f.begin(f.primary);f.tracker.execute(f.primary,1,&f.secondary);f.tracker.end(f.primary,true);f.tracker.reset(f.secondary);f.submit();
        check(!f.last().exclusiveObservedRanges&&f.last().status=="stale_or_invalid_command_recording","Reset secondary was attributed");++cases;
    }
    {
        Fixture f;f.begin(f.primary);f.draw(f.primary);f.tracker.end(f.primary,true);f.tracker.resetPool(f.device,f.pool);f.submit();
        check(!f.last().exclusiveObservedRanges,"Reset pool retained proof");++cases;
    }
    {
        Fixture f;f.tracker.allocation({},reinterpret_cast<std::uintptr_t>(f.storage.data()),f.storage.size());f.simple();
        check(!f.last().exclusiveObservedRanges,"Foreign interface address reuse inherited HUD identity");++cases;
    }
    {
        Fixture f;f.tracker.ownGui(101,1,30,1);auto other=f.tracker.beginGui(101,30,1);
        f.tracker.allocation(other,reinterpret_cast<std::uintptr_t>(f.storage.data()+2*48),2*48);f.tracker.endGui(other);f.simple();
        check(!f.last().exclusiveObservedRanges,"Mixed-interface batch was attributed to one GUI");++cases;
    }
    {
        Fixture f;f.tracker.ownGui(101,1,30,1);auto other=f.tracker.beginGui(101,30,1);
        f.tracker.allocation(other,reinterpret_cast<std::uintptr_t>(f.storage.data()+4*48),4*48);f.tracker.endGui(other);
        // The actual indices, rather than the complete buffer or a draw's
        // contiguous min/max range, select only the first GUI allocation.
        // Restore the first non-overlapping allocation invalidated conservatively.
        f.tracker.allocation(f.source,reinterpret_cast<std::uintptr_t>(f.storage.data()),4*48);f.simple();
        check(f.last().exclusiveObservedRanges&&f.last().source.gui==100,"Shared buffer disjoint draw not resolved by indices");++cases;
    }
    {
        Fixture f;f.tracker.ownGui(100,0,31,1);auto next=f.tracker.beginGui(100,31,1);f.tracker.endGui(next);f.simple();
        check(!f.last().exclusiveObservedRanges,"Old GUI generation survived reuse");++cases;
    }
    {
        Fixture f;f.tracker.ownGui(100,0,32,2);f.simple();check(!f.last().exclusiveObservedRanges,"Old level survived address reuse");
        f.tracker.ownGui(100,0,30,1);check(!f.tracker.beginGui(100,30,1),"Stale job rewound the level");++cases;
    }
    {
        Fixture f;f.begin(f.primary);f.draw(f.primary);f.tracker.end(f.primary,true);
        f.tracker.destroyBuffer(f.device,f.vertices);VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=f.storage.size();info.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        f.tracker.createBuffer(f.device,f.vertices,info);f.tracker.bindBufferMemory(f.device,f.vertices,f.vertexMemory,0);f.submit();
        check(!f.last().exclusiveObservedRanges,"Retired vertex-buffer handle was accepted");++cases;
    }
    {
        Fixture f;f.begin(f.primary);f.draw(f.primary);f.tracker.end(f.primary,true);f.tracker.destroyPipeline(f.device,f.pipeline);f.submit();
        check(!f.last().exclusiveObservedRanges,"Retired pipeline survived");++cases;
    }
    {
        Fixture f;f.begin(f.primary);f.tracker.indirect(f.primary,f.indices,0,1,20,true);f.tracker.end(f.primary,true);f.submit();
        check(!f.last().exclusiveObservedRanges,"Unknown GPU indirect parameters were guessed");++cases;
    }
    {
        Fixture f;f.indexData[2]=65535;f.simple();check(!f.last().exclusiveObservedRanges,"Out of range index was attributed");++cases;
    }
    {
        Fixture f;std::array<unsigned char,48*8> identical=f.storage;
        f.tracker.unmapMemory(f.device,f.vertexMemory);f.tracker.mapMemory(f.device,f.vertexMemory,0,identical.size(),identical.data());f.simple();
        check(!f.last().exclusiveObservedRanges,"Unobserved CPU copy was inferred from equal bytes");++cases;
    }
    {
        Fixture f;VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=f.storage.size();info.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        f.tracker.createBuffer(f.device,f.copied,info);
        // Record consumer first on another command buffer. Attribute only
        // after the explicit producer copy executes before the secondary.
        f.begin(f.secondary,f.copied);f.draw(f.secondary);f.tracker.end(f.secondary,true);
        f.begin(f.primary);VkBufferCopy copy{0,0,f.storage.size()};f.tracker.copy(f.primary,f.vertices,f.copied,1,&copy);
        f.tracker.execute(f.primary,1,&f.secondary);f.tracker.end(f.primary,true);f.submit();
        check(f.last().exclusiveObservedRanges,"Observed GPU copy did not carry exact byte lineage");++cases;
    }
    {
        Fixture f;f.begin(f.primary);std::array<unsigned char,48*4> data{};
        f.tracker.update(f.primary,f.vertices,0,data.size(),data.data());f.draw(f.primary);f.tracker.end(f.primary,true);f.submit();
        check(!f.last().exclusiveObservedRanges,"Unknown GPU writer retained old ownership");++cases;
    }
    {
        Fixture f;f.begin(f.primary);f.draw(f.primary);f.tracker.end(f.primary,true);
        VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=f.primary;
        info.commandBufferInfoCount=1;info.pCommandBufferInfos=&cb;f.tracker.submit2(handle<VkQueue>(50),1,&info,VK_NULL_HANDLE,true);
        check(f.last().exclusiveObservedRanges,"Submit2 lost recording dependency");++cases;
    }
    {
        Fixture f;f.begin(f.primary);f.draw(f.primary);f.tracker.end(f.primary,true);
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&f.primary;
        f.tracker.submit(handle<VkQueue>(50),1,&info,VK_NULL_HANDLE,false);check(f.tracker.findings().empty(),"Failed submission treated as recorded execution");++cases;
    }
    if(argc>1){Fixture f;f.simple();std::cout<<f.tracker.report()<<'\n';}
    else std::cout<<cases<<" provenance scenarios passed; replay remains disabled; no temporal or content-fingerprint attribution\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
