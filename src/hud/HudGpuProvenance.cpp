#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "HudGpuProvenance.h"
#include "../common/RuntimePaths.h"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <type_traits>

namespace kharvox::hudtrace {
namespace {
template<class T> std::uint64_t id(T value){
    if constexpr(std::is_pointer_v<T>)return reinterpret_cast<std::uintptr_t>(value);
    else return static_cast<std::uint64_t>(value);
}
using Key=std::pair<std::uint64_t,std::uint64_t>;
bool fits(std::uint64_t off,std::uint64_t size,std::uint64_t limit){return off<=limit&&size<=limit-off;}
std::string quote(const std::string& value){std::string result="\"";for(unsigned char c:value){
    if(c=='\\'||c=='\"'){result+='\\';result+=char(c);}else if(c>=32&&c<127)result+=char(c);
    else {const char* h="0123456789abcdef";result+="\\u00";result+=h[c>>4];result+=h[c&15];}}
    return result+'\"';}
std::string hex(const void* data,size_t bytes){const auto* p=static_cast<const unsigned char*>(data);
    std::string text;const char* h="0123456789abcdef";for(size_t i=0;i<bytes;++i){text+=h[p[i]>>4];text+=h[p[i]&15];}return quote(text);}
template<class T> std::string arrayBytes(const T* data,unsigned count){
    return data&&count<=4096?hex(data,sizeof(T)*count):"null";
}
std::string sourceJson(const Source& s){std::ostringstream o;o<<"{\"event\":"<<s.event<<",\"gui\":"<<s.gui
    <<",\"generation\":"<<s.generation<<",\"level\":"<<s.level<<",\"presentMetadataOnly\":"<<s.present<<",\"panel\":"<<s.panel<<'}';return o.str();}
bool readBytes(std::uint64_t address,void* destination,size_t bytes){
    if(!address||!fits(address,bytes,std::numeric_limits<std::uintptr_t>::max()))return false;
    auto cursor=address;const auto end=address+bytes;
    while(cursor<end){MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(cursor),&info,sizeof(info))||info.State!=MEM_COMMIT
            ||(info.Protect&(PAGE_GUARD|PAGE_NOACCESS))||!(info.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY
                |PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
        const auto next=id(info.BaseAddress)+info.RegionSize;if(next<=cursor)return false;cursor=std::min(end,next);
    }
    std::memcpy(destination,reinterpret_cast<void*>(address),bytes);return true;
}
thread_local Source currentSource;
unsigned formatBytes(VkFormat format){switch(format){
case VK_FORMAT_R32G32B32A32_SFLOAT:return 16;case VK_FORMAT_R32G32B32_SFLOAT:return 12;
case VK_FORMAT_R32G32_SFLOAT:return 8;case VK_FORMAT_R32_SFLOAT:return 4;
case VK_FORMAT_R16G16_UNORM:case VK_FORMAT_R16G16_SNORM:case VK_FORMAT_R16G16_SFLOAT:
case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_R8G8B8A8_UINT:case VK_FORMAT_R8G8B8A8_SNORM:return 4;
default:return 0;}}
}

struct Tracker::Impl {
    mutable std::mutex mutex;
    std::uint64_t serial{},submission{},currentLevel{},cpuEvents{},allocations{},unmappedAllocations{},draws{},resultBytes{};
    bool levelKnown{},overflow{};
    struct Gui {int panel{-1};std::uint64_t level{},present{},generation{};bool complete{};};
    struct Span {std::uint64_t begin{},size{};Source source;};
    struct Memory {std::uint64_t size{},offset{},mappedSize{},address{},generation{};};
    struct Buffer {std::uint64_t size{},memory{},memoryOffset{},generation{};VkBufferUsageFlags usage{};
        std::vector<Span> transferred;std::map<std::uint64_t,std::vector<unsigned char>> shadow;};
    struct Pipeline {std::uint64_t generation{};std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;std::string description;};
    struct Descriptor {std::uint64_t version{};std::map<Key,std::string> slots;bool unsupported{};};
    struct State {std::uint64_t pipeline{},pipelineGeneration{},index{};VkDeviceSize indexOffset{};VkIndexType indexType{VK_INDEX_TYPE_MAX_ENUM};
        std::map<unsigned,std::pair<std::uint64_t,VkDeviceSize>> vertices;
        std::map<unsigned,std::uint64_t> vertexGenerations,descriptorSets;
        std::uint64_t indexGeneration{};
        std::vector<std::string> descriptorHistory,pushHistory,dynamicHistory,barriers;
        std::uint64_t renderPass{},framebuffer{};unsigned subpass{};bool incomplete{};};
    struct Event {enum Kind {Draw,Copy,Update,Execute} kind{Draw};State state;
        unsigned count{},instances{},first{},firstInstance{},stride{};int vertexOffset{};bool indexed{},indirect{};
        std::uint64_t buffer{},other{},offset{},childRecording{};VkBufferCopy region{};
        std::vector<unsigned char> bytes;};
    struct Command {std::uint64_t device{},pool{},recording{};VkCommandBufferLevel level{};
        bool recordingNow{},executable{};State state;std::vector<Event> events;std::string beginDescription;};
    std::map<std::uint64_t,Gui> guis;
    std::vector<Span> spans;
    std::map<Key,Memory> memories;
    std::map<Key,Buffer> buffers;
    std::map<Key,Pipeline> pipelines;
    std::map<Key,Descriptor> descriptors;
    std::map<std::uint64_t,Command> commands;
    std::vector<Finding> results;
    std::vector<std::string> cpuLog,submissionLog;
    static constexpr size_t maximumEvents=4096,maximumSpans=4096;

    bool alive(const Source& s)const {auto g=guis.find(s.gui);return bool(s)&&g!=guis.end()
        &&g->second.level==s.level&&g->second.generation==s.generation&&g->second.complete;}
    static void replace(std::vector<Span>& list,const Span& value){
        // Conservative invalidation of overlapping entries, including foreign
        // writes. Never retain an old positive label across an unknown writer.
        list.erase(std::remove_if(list.begin(),list.end(),[&](const Span& old){
            return value.begin<old.begin+old.size&&old.begin<value.begin+value.size;}),list.end());
        if(value.source)list.push_back(value);
    }
    std::uint64_t address(Key key,std::uint64_t off,std::uint64_t bytes)const {
        auto b=buffers.find(key);if(b==buffers.end()||!fits(off,bytes,b->second.size))return 0;
        auto m=memories.find({key.first,b->second.memory});if(m==memories.end()||!m->second.address)return 0;
        const auto bound=b->second.memoryOffset;if(!fits(bound,off,std::numeric_limits<std::uint64_t>::max()))return 0;
        const auto absolute=bound+off;const auto& map=m->second;
        if(absolute<map.offset||!fits(absolute-map.offset,bytes,map.mappedSize))return 0;
        return map.address+absolute-map.offset;
    }
    Source owner(Key key,std::uint64_t off,std::uint64_t bytes)const {
        auto b=buffers.find(key);if(b==buffers.end()||!fits(off,bytes,b->second.size))return {};
        for(const auto& span:b->second.transferred)if(off>=span.begin&&fits(off-span.begin,bytes,span.size))
            return alive(span.source)?span.source:Source{};
        auto pointer=address(key,off,bytes);if(!pointer)return {};
        for(const auto& span:spans)if(pointer>=span.begin&&fits(pointer-span.begin,bytes,span.size))
            return alive(span.source)?span.source:Source{};
        return {};
    }
    bool read(Key key,std::uint64_t off,void* output,size_t bytes)const {
        auto b=buffers.find(key);if(b==buffers.end())return false;
        for(const auto& entry:b->second.shadow)if(off>=entry.first&&fits(off-entry.first,bytes,entry.second.size())){
            std::memcpy(output,entry.second.data()+off-entry.first,bytes);return true;}
        auto pointer=address(key,off,bytes);return pointer&&readBytes(pointer,output,bytes);
    }
    Command* command(VkCommandBuffer cb){auto c=commands.find(id(cb));return c==commands.end()?nullptr:&c->second;}
    void add(Command& c,Event e){if(c.events.size()>=maximumEvents){overflow=true;c.state.incomplete=true;return;}c.events.push_back(std::move(e));}
    template<class F> void state(VkCommandBuffer cb,F action){auto c=command(cb);if(c&&c->recordingNow)action(*c);}
    void inspect(std::uint64_t cb,Command& c,const Event& e,unsigned ordinal,const std::string& ancestry){
        if(results.size()>=maximumEvents||resultBytes>=8*1024*1024){overflow=true;return;}
        Finding f;f.commandBuffer=cb;f.recording=c.recording;f.ordinal=ordinal;f.submission=submission;
        f.status="unresolved";std::string reason;
        auto p=pipelines.find({c.device,e.state.pipeline});
        if(e.indirect)reason="indirect arguments not resolved";
        else if(e.state.incomplete||overflow)reason="bounded trace incomplete";
        else if(p==pipelines.end()||p->second.generation!=e.state.pipelineGeneration)reason="missing or retired pipeline generation";
        else if(!e.count||!e.instances)reason="empty draw";
        else if(p->second.bindings.size()!=1||p->second.bindings[0].stride!=48
            ||p->second.bindings[0].inputRate!=VK_VERTEX_INPUT_RATE_VERTEX)reason="native GUI 48-byte vertex layout not established";
        else if(p->second.attributes.empty()||std::any_of(p->second.attributes.begin(),p->second.attributes.end(),[&](const auto& a){
            const auto bytes=formatBytes(a.format);return !bytes||a.binding!=p->second.bindings[0].binding||!fits(a.offset,bytes,48);}))
            reason="vertex fetch footprint not established";
        else {
            const auto binding=p->second.bindings[0].binding;auto vertex=e.state.vertices.find(binding);
            if(vertex==e.state.vertices.end())reason="vertex binding missing";
            else if(buffers.find({c.device,vertex->second.first})==buffers.end()
                ||!e.state.vertexGenerations.count(binding)
                ||buffers.at({c.device,vertex->second.first}).generation!=e.state.vertexGenerations.at(binding))reason="retired vertex buffer generation";
            else if(e.indexed&&(buffers.find({c.device,e.state.index})==buffers.end()
                ||buffers.at({c.device,e.state.index}).generation!=e.state.indexGeneration))reason="retired index buffer generation";
            else if(e.count>65536)reason="index/vertex audit limit exceeded";
            else {Source only;
                for(unsigned i=0;i<e.count&&reason.empty();++i){
                    std::int64_t index=std::int64_t(e.first)+i;
                    if(e.indexed){
                        if(e.state.indexType==VK_INDEX_TYPE_UINT16){std::uint16_t value{};
                            if(!read({c.device,e.state.index},e.state.indexOffset+index*2,&value,2)){reason="index bytes unavailable";break;}index=value;
                        }else if(e.state.indexType==VK_INDEX_TYPE_UINT32){std::uint32_t value{};
                            if(!read({c.device,e.state.index},e.state.indexOffset+index*4,&value,4)){reason="index bytes unavailable";break;}index=value;
                        }else {reason="unsupported index type";break;}
                        index+=e.vertexOffset;
                    }
                    if(index<0||std::uint64_t(index)>(std::numeric_limits<std::uint64_t>::max()-vertex->second.second)/48){reason="vertex address overflow";break;}
                    auto source=owner({c.device,vertex->second.first},vertex->second.second+std::uint64_t(index)*48,48);
                    if(!source){reason="unowned/stale vertex bytes or unobserved upload";break;}
                    if(only&&only.event!=source.event){reason="mixed GUI/event draw";break;}only=source;
                }
                if(reason.empty()){f.source=only;f.exclusiveObservedRanges=true;f.status="exclusive_observed_allocation_lineage";}
            }
        }
        std::ostringstream o;o<<"{\"commandBuffer\":"<<cb<<",\"recording\":"<<c.recording<<",\"ordinal\":"<<ordinal
            <<",\"submission\":"<<submission<<",\"ownership\":"<<quote(f.status)<<",\"reason\":"<<quote(reason)
            <<",\"source\":"<<sourceJson(f.source)<<",\"replaySafe\":false,\"ancestry\":"<<ancestry
            <<",\"pipeline\":"<<e.state.pipeline<<",\"pipelineGeneration\":"<<e.state.pipelineGeneration
            <<",\"pipelineCreateState\":"<<(p==pipelines.end()?"null":p->second.description)
            <<",\"renderPass\":"<<e.state.renderPass<<",\"subpass\":"<<e.state.subpass<<",\"framebuffer\":"<<e.state.framebuffer
            <<",\"draw\":{\"indexed\":"<<e.indexed<<",\"indirect\":"<<e.indirect<<",\"count\":"<<e.count
            <<",\"instances\":"<<e.instances<<",\"first\":"<<e.first<<",\"vertexOffset\":"<<e.vertexOffset
            <<",\"firstInstance\":"<<e.firstInstance<<",\"argumentBuffer\":"<<e.buffer<<",\"argumentOffset\":"<<e.offset<<",\"stride\":"<<e.stride<<'}';
        o<<",\"vertexBindings\":[";bool comma=false;for(auto b:e.state.vertices){if(comma)o<<',';comma=true;
            auto resource=buffers.find({c.device,b.second.first});o<<"{\"binding\":"<<b.first<<",\"buffer\":"<<b.second.first
                <<",\"offset\":"<<b.second.second<<",\"generation\":"<<(resource==buffers.end()?0:resource->second.generation)<<'}';}o<<']';
        o<<",\"indexBuffer\":"<<e.state.index<<",\"indexOffset\":"<<e.state.indexOffset<<",\"indexType\":"<<e.state.indexType;
        auto strings=[&](const char* name,const std::vector<std::string>& list){o<<",\""<<name<<"\":[";for(size_t i=0;i<list.size();++i){if(i)o<<',';o<<list[i];}o<<']';};
        strings("descriptorBindHistory",e.state.descriptorHistory);strings("pushConstantWrites",e.state.pushHistory);
        strings("dynamicStateWrites",e.state.dynamicHistory);strings("observedBarriers",e.state.barriers);
        o<<",\"descriptorContentsAtSubmission\":[";comma=false;for(const auto& d:descriptors)if(d.first.first==c.device
            &&std::any_of(e.state.descriptorSets.begin(),e.state.descriptorSets.end(),[&](const auto& v){return v.second==d.first.second;})){if(comma)o<<',';comma=true;
            o<<"{\"set\":"<<d.first.second<<",\"observedVersion\":"<<d.second.version<<",\"unsupportedOrCopied\":"<<d.second.unsupported<<",\"slots\":[";
            bool slotComma=false;for(const auto& s:d.second.slots){if(slotComma)o<<',';slotComma=true;o<<s.second;}o<<"]}";}o<<"]}";
        f.details=o.str();resultBytes+=f.details.size();results.push_back(std::move(f));++draws;
    }
    void walk(std::uint64_t cb,std::uint64_t recording,const std::string& ancestry,unsigned depth=0){
        auto found=commands.find(cb);if(depth>16||found==commands.end()||!found->second.executable||found->second.recording!=recording){
            Finding f;f.commandBuffer=cb;f.recording=recording;f.submission=submission;f.status="stale_or_invalid_command_recording";
            f.details="{\"ownership\":\"stale_or_invalid_command_recording\",\"replaySafe\":false}";
            if(results.size()<maximumEvents)results.push_back(f);else overflow=true;return;}
        auto& c=found->second;
        for(unsigned i=0;i<c.events.size();++i){auto& e=c.events[i];
            if(e.kind==Event::Draw)inspect(cb,c,e,i,ancestry);
            else if(e.kind==Event::Execute){std::ostringstream a;a<<"{\"parent\":"<<cb<<",\"parentRecording\":"<<recording
                <<",\"executeOrdinal\":"<<i<<",\"renderPass\":"<<e.state.renderPass<<",\"subpass\":"<<e.state.subpass
                <<",\"framebuffer\":"<<e.state.framebuffer<<",\"parentContext\":"<<ancestry<<'}';walk(e.buffer,e.childRecording,a.str(),depth+1);}
            else {auto destination=buffers.find({c.device,e.other});if(destination==buffers.end())continue;
                auto& b=destination->second;const auto off=e.kind==Event::Copy?e.region.dstOffset:e.offset;
                const auto size=e.kind==Event::Copy?e.region.size:e.bytes.size();
                const auto destinationAddress=address({c.device,e.other},off,size);
                if(destinationAddress)replace(spans,{destinationAddress,size,{}});
                replace(b.transferred,{off,size,{}});b.shadow.clear();
                if(e.kind==Event::Copy){
                    // Propagate identity through an observed GPU byte-copy,
                    // in submission order, never through a content hash.
                    auto source=buffers.find({c.device,e.buffer});if(source==buffers.end())continue;
                    std::vector<Span> candidates=source->second.transferred;
                    const auto mapped=address({c.device,e.buffer},e.region.srcOffset,size);
                    if(mapped)for(const auto& s:spans){const auto lo=std::max(mapped,s.begin),hi=std::min(mapped+size,s.begin+s.size);
                        if(hi>lo)candidates.push_back({e.region.srcOffset+lo-mapped,hi-lo,s.source});}
                    for(const auto& s:candidates){const auto lo=std::max(e.region.srcOffset,s.begin),hi=std::min(e.region.srcOffset+size,s.begin+s.size);
                        if(hi>lo&&alive(s.source)&&b.transferred.size()<maximumSpans)b.transferred.push_back({off+lo-e.region.srcOffset,hi-lo,s.source});}
                    if((b.usage&VK_BUFFER_USAGE_INDEX_BUFFER_BIT)&&size<=1024*1024){std::vector<unsigned char> bytes(size);
                        if(read({c.device,e.buffer},e.region.srcOffset,bytes.data(),bytes.size()))b.shadow[off]=std::move(bytes);}
                }else b.shadow[off]=e.bytes;
            }
        }
    }
};

Tracker::Tracker():impl_(std::make_unique<Impl>()){} Tracker::~Tracker()=default;
#define LOCK auto& s=*impl_;std::lock_guard<std::mutex> guard(s.mutex)
void Tracker::ownGui(std::uint64_t gui,int panel,std::uint64_t present,std::uint64_t level){LOCK;
    if(!gui||panel<0||panel>2)return;
    if(s.levelKnown&&level<s.currentLevel)return;
    if(!s.levelKnown||s.currentLevel!=level){s.guis.clear();s.spans.clear();s.currentLevel=level;s.levelKnown=true;}
    auto& g=s.guis[gui];if(g.panel!=panel||g.level!=level){g={};g.panel=panel;g.level=level;}g.present=present;
}
Source Tracker::beginGui(std::uint64_t gui,std::uint64_t present,std::uint64_t level){LOCK;
    auto found=s.guis.find(gui);if(found==s.guis.end()||found->second.level!=level
        ||present<found->second.present||present-found->second.present>1)return {};
    if(s.cpuLog.size()>=256){s.overflow=true;return {};}
    auto& g=found->second;g.generation=++s.serial;g.complete=false;
    Source source{++s.serial,gui,g.generation,level,present,g.panel};
    s.cpuLog.push_back("{\"kind\":\"gui_emission_begin\",\"source\":"+sourceJson(source)+'}');++s.cpuEvents;return source;
}
void Tracker::endGui(const Source& source){LOCK;auto g=s.guis.find(source.gui);
    if(g!=s.guis.end()&&g->second.generation==source.generation){g->second.complete=true;
        if(s.cpuLog.size()<256)s.cpuLog.push_back("{\"kind\":\"gui_emission_end\",\"source\":"+sourceJson(source)+'}');}
}
void Tracker::allocation(const Source& source,std::uint64_t address,std::uint64_t bytes,std::uint64_t material,std::uint64_t inputIndices,unsigned indexCount){LOCK;
    if(!address||!bytes||!fits(address,bytes,std::numeric_limits<std::uint64_t>::max()))return;
    Impl::replace(s.spans,{address,bytes,{}});
    if(s.spans.size()>=Impl::maximumSpans){s.overflow=true;return;}
    if(source)s.spans.push_back({address,bytes,source});++s.allocations;
    bool aliased=false;for(auto& b:s.buffers){const auto start=s.address(b.first,0,b.second.size);
        aliased|=start&&address>=start&&fits(address-start,bytes,b.second.size);}
    if(source&&!aliased)++s.unmappedAllocations;
    if(s.cpuLog.size()<256&&source){std::ostringstream o;o<<"{\"kind\":\"native_allocation\",\"source\":"<<sourceJson(source)
        <<",\"address\":"<<address<<",\"bytes\":"<<bytes<<",\"material\":"<<material
        <<",\"inputIndicesNotRebasedGpuIndices\":"<<inputIndices<<",\"inputIndexCount\":"<<indexCount
        <<",\"currentlyAliasesMappedBuffer\":"<<aliased<<'}';s.cpuLog.push_back(o.str());}
}
void Tracker::allocateMemory(VkDevice d,VkDeviceMemory m,VkDeviceSize bytes){LOCK;s.memories[{id(d),id(m)}]={bytes,0,0,0,++s.serial};}
void Tracker::freeMemory(VkDevice d,VkDeviceMemory m){LOCK;s.memories.erase({id(d),id(m)});for(auto& b:s.buffers)
    if(b.first.first==id(d)&&b.second.memory==id(m)){b.second.memory=0;b.second.transferred.clear();b.second.shadow.clear();}}
void Tracker::mapMemory(VkDevice d,VkDeviceMemory m,VkDeviceSize off,VkDeviceSize bytes,void* pointer){LOCK;auto found=s.memories.find({id(d),id(m)});
    if(found==s.memories.end())return;auto& memory=found->second;const auto count=bytes==VK_WHOLE_SIZE?memory.size-off:bytes;
    if(!fits(off,count,memory.size))return;memory.offset=off;memory.mappedSize=count;memory.address=id(pointer);}
void Tracker::unmapMemory(VkDevice d,VkDeviceMemory m){LOCK;auto found=s.memories.find({id(d),id(m)});if(found!=s.memories.end())found->second.address=0;}
void Tracker::createBuffer(VkDevice d,VkBuffer b,const VkBufferCreateInfo& info){LOCK;Impl::Buffer value;value.size=info.size;value.usage=info.usage;value.generation=++s.serial;s.buffers[{id(d),id(b)}]=value;}
void Tracker::destroyBuffer(VkDevice d,VkBuffer b){LOCK;s.buffers.erase({id(d),id(b)});}
void Tracker::bindBufferMemory(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize off){LOCK;auto found=s.buffers.find({id(d),id(b)});
    if(found!=s.buffers.end()){found->second.memory=id(m);found->second.memoryOffset=off;found->second.transferred.clear();found->second.shadow.clear();}}
void Tracker::allocateCommands(VkDevice d,VkCommandPool pool,VkCommandBufferLevel level,unsigned count,const VkCommandBuffer* values){LOCK;
    for(unsigned i=0;values&&i<count;++i){Impl::Command c;c.device=id(d);c.pool=id(pool);c.level=level;s.commands[id(values[i])]=c;}}
void Tracker::freeCommands(unsigned count,const VkCommandBuffer* values){LOCK;for(unsigned i=0;values&&i<count;++i)s.commands.erase(id(values[i]));}
void Tracker::resetPool(VkDevice d,VkCommandPool pool){LOCK;for(auto& c:s.commands)if(c.second.device==id(d)&&c.second.pool==id(pool)){
    c.second.executable=false;c.second.recordingNow=false;c.second.events.clear();c.second.recording=++s.serial;}}
void Tracker::begin(VkCommandBuffer cb,const VkCommandBufferBeginInfo& info){LOCK;auto c=s.command(cb);if(!c)return;
    c->recording=++s.serial;c->executable=false;c->recordingNow=true;c->events.clear();c->state={};
    if(info.pInheritanceInfo){c->state.renderPass=id(info.pInheritanceInfo->renderPass);c->state.framebuffer=id(info.pInheritanceInfo->framebuffer);
        c->state.subpass=info.pInheritanceInfo->subpass;c->state.incomplete=info.pInheritanceInfo->pNext!=nullptr;}
    c->state.incomplete|=info.pNext!=nullptr;
}
void Tracker::end(VkCommandBuffer cb,bool success){LOCK;auto c=s.command(cb);if(c){c->recordingNow=false;c->executable=success;}}
void Tracker::reset(VkCommandBuffer cb){LOCK;auto c=s.command(cb);if(c){c->recording=++s.serial;c->recordingNow=false;c->executable=false;c->events.clear();}}
void Tracker::execute(VkCommandBuffer cb,unsigned count,const VkCommandBuffer* children){LOCK;s.state(cb,[&](auto& c){for(unsigned i=0;children&&i<count;++i){
    Impl::Event e;e.kind=Impl::Event::Execute;e.buffer=id(children[i]);auto child=s.command(children[i]);e.childRecording=child?child->recording:0;e.state=c.state;s.add(c,e);}});}

void Tracker::createPipelines(VkDevice device,unsigned count,const VkGraphicsPipelineCreateInfo* infos,const VkPipeline* values){LOCK;
    for(unsigned i=0;infos&&values&&i<count;++i){if(!values[i])continue;if(s.pipelines.size()>=1024){s.overflow=true;break;}
        const auto& info=infos[i];Impl::Pipeline p;p.generation=++s.serial;
        if(info.pVertexInputState){const auto& v=*info.pVertexInputState;
            if(v.pVertexBindingDescriptions&&v.vertexBindingDescriptionCount<=16)p.bindings.assign(v.pVertexBindingDescriptions,v.pVertexBindingDescriptions+v.vertexBindingDescriptionCount);
            if(v.pVertexAttributeDescriptions&&v.vertexAttributeDescriptionCount<=32)p.attributes.assign(v.pVertexAttributeDescriptions,v.pVertexAttributeDescriptions+v.vertexAttributeDescriptionCount);}
        std::ostringstream o;o<<"{\"pipelineLayout\":"<<id(info.layout)<<",\"renderPass\":"<<id(info.renderPass)<<",\"subpass\":"<<info.subpass
            <<",\"flags\":"<<info.flags<<",\"pNextPresent\":"<<(info.pNext!=nullptr)<<",\"abi\":\"Win64 Vulkan structures, raw bytes include opaque pNext addresses only\""
            <<",\"vertexBindings\":"<<arrayBytes(p.bindings.data(),unsigned(p.bindings.size()))<<",\"vertexAttributes\":"<<arrayBytes(p.attributes.data(),unsigned(p.attributes.size()));
        auto state=[&](const char* name,const auto* value){o<<",\""<<name<<"\":"<<(value?hex(value,sizeof(*value)):"null");};
        state("inputAssembly",info.pInputAssemblyState);state("tessellation",info.pTessellationState);
        state("rasterization",info.pRasterizationState);state("depthStencil",info.pDepthStencilState);state("multisample",info.pMultisampleState);
        if(info.pMultisampleState&&info.pMultisampleState->pSampleMask)o<<",\"sampleMask\":"<<arrayBytes(info.pMultisampleState->pSampleMask,(unsigned(info.pMultisampleState->rasterizationSamples)+31)/32);
        if(info.pViewportState){o<<",\"viewports\":"<<arrayBytes(info.pViewportState->pViewports,info.pViewportState->viewportCount)
            <<",\"scissors\":"<<arrayBytes(info.pViewportState->pScissors,info.pViewportState->scissorCount);}
        if(info.pColorBlendState){state("colorBlendHeader",info.pColorBlendState);o<<",\"blendAttachments\":"<<arrayBytes(info.pColorBlendState->pAttachments,info.pColorBlendState->attachmentCount);}
        if(info.pDynamicState)o<<",\"dynamicStateRequirements\":"<<arrayBytes(info.pDynamicState->pDynamicStates,info.pDynamicState->dynamicStateCount);
        o<<",\"shaderStages\":[";for(unsigned j=0;info.pStages&&j<info.stageCount&&j<16;++j){if(j)o<<',';const auto& stage=info.pStages[j];
            o<<"{\"module\":"<<id(stage.module)<<",\"stage\":"<<stage.stage<<",\"entry\":"<<quote(stage.pName?stage.pName:"")<<",\"pNextPresent\":"<<(stage.pNext!=nullptr);
            if(stage.pSpecializationInfo){const auto& sp=*stage.pSpecializationInfo;o<<",\"specializationMap\":"<<arrayBytes(sp.pMapEntries,sp.mapEntryCount)
                <<",\"specializationBytes\":"<<(sp.pData&&sp.dataSize<=4096?hex(sp.pData,sp.dataSize):"null");}o<<'}';}o<<"]}";
        p.description=o.str();s.pipelines[{id(device),id(values[i])}]=std::move(p);
    }
}
void Tracker::destroyPipeline(VkDevice d,VkPipeline p){LOCK;s.pipelines.erase({id(d),id(p)});}
void Tracker::descriptors(VkDevice device,unsigned count,const VkWriteDescriptorSet* writes,unsigned copyCount,const VkCopyDescriptorSet* copies){LOCK;
    for(unsigned i=0;writes&&i<count;++i){const auto& w=writes[i];if(s.descriptors.size()>=4096){s.overflow=true;return;}
        auto& d=s.descriptors[{id(device),id(w.dstSet)}];d.version=++s.serial;d.unsupported|=w.pNext!=nullptr||w.descriptorCount>256;
        for(unsigned n=0;n<std::min(w.descriptorCount,256u);++n){std::ostringstream o;o<<"{\"binding\":"<<w.dstBinding<<",\"arrayElement\":"<<w.dstArrayElement+n<<",\"type\":"<<w.descriptorType;
            if(w.pImageInfo)o<<",\"sampler\":"<<id(w.pImageInfo[n].sampler)<<",\"imageView\":"<<id(w.pImageInfo[n].imageView)<<",\"layout\":"<<w.pImageInfo[n].imageLayout;
            if(w.pBufferInfo)o<<",\"buffer\":"<<id(w.pBufferInfo[n].buffer)<<",\"offset\":"<<w.pBufferInfo[n].offset<<",\"range\":"<<w.pBufferInfo[n].range;
            if(w.pTexelBufferView)o<<",\"texelView\":"<<id(w.pTexelBufferView[n]);o<<'}';d.slots[{w.dstBinding,w.dstArrayElement+n}]=o.str();}}
    for(unsigned i=0;copies&&i<copyCount;++i){auto& d=s.descriptors[{id(device),id(copies[i].dstSet)}];d.version=++s.serial;d.unsupported=true;
        // Descriptor binding rollover cannot be reconstructed without layout
        // declarations. Preserve the raw dependency; do not invent contents.
        d.slots[{copies[i].dstBinding,copies[i].dstArrayElement}]="{\"unresolvedDescriptorCopy\":"+hex(copies+i,sizeof(copies[i]))+'}';}
}
void Tracker::bindDescriptors(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipelineLayout layout,unsigned first,unsigned count,
    const VkDescriptorSet* values,unsigned dynamicCount,const std::uint32_t* offsets){LOCK;if(point!=VK_PIPELINE_BIND_POINT_GRAPHICS)return;
    s.state(cb,[&](auto& c){std::ostringstream o;o<<"{\"pipelineLayout\":"<<id(layout)<<",\"firstSet\":"<<first<<",\"sets\":[";
        for(unsigned i=0;values&&i<count;++i){if(i)o<<',';o<<id(values[i]);if(first+i<16)c.state.descriptorSets[first+i]=id(values[i]);else c.state.incomplete=true;}o<<"],\"dynamicOffsets\":"<<arrayBytes(offsets,dynamicCount)<<'}';
        if(c.state.descriptorHistory.size()<128)c.state.descriptorHistory.push_back(o.str());else c.state.incomplete=true;});}
void Tracker::bindPipeline(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipeline pipeline){LOCK;if(point!=VK_PIPELINE_BIND_POINT_GRAPHICS)return;
    s.state(cb,[&](auto& c){c.state.pipeline=id(pipeline);auto p=s.pipelines.find({c.device,id(pipeline)});c.state.pipelineGeneration=p==s.pipelines.end()?0:p->second.generation;});}
void Tracker::bindVertices(VkCommandBuffer cb,unsigned first,unsigned count,const VkBuffer* values,const VkDeviceSize* offsets){LOCK;
    s.state(cb,[&](auto& c){for(unsigned i=0;values&&i<count&&i<16;++i){c.state.vertices[first+i]={id(values[i]),offsets?offsets[i]:0};
        auto b=s.buffers.find({c.device,id(values[i])});c.state.vertexGenerations[first+i]=b==s.buffers.end()?0:b->second.generation;}if(count>16)c.state.incomplete=true;});}
void Tracker::bindIndex(VkCommandBuffer cb,VkBuffer value,VkDeviceSize off,VkIndexType type){LOCK;s.state(cb,[&](auto& c){c.state.index=id(value);c.state.indexOffset=off;c.state.indexType=type;
    auto b=s.buffers.find({c.device,id(value)});c.state.indexGeneration=b==s.buffers.end()?0:b->second.generation;});}
void Tracker::push(VkCommandBuffer cb,VkPipelineLayout layout,VkShaderStageFlags stages,unsigned off,unsigned bytes,const void* data){LOCK;s.state(cb,[&](auto& c){
    if(bytes>256||!data||c.state.pushHistory.size()>=128){c.state.incomplete=true;return;}
    std::ostringstream o;o<<"{\"layout\":"<<id(layout)<<",\"stages\":"<<stages<<",\"offset\":"<<off<<",\"bytes\":"<<hex(data,bytes)<<'}';c.state.pushHistory.push_back(o.str());});}
void Tracker::viewport(VkCommandBuffer cb,unsigned first,unsigned count,const VkViewport* values){LOCK;s.state(cb,[&](auto& c){
    if(count>16||c.state.dynamicHistory.size()>=128){c.state.incomplete=true;return;}c.state.dynamicHistory.push_back("{\"viewportFirst\":"+std::to_string(first)+",\"bytes\":"+arrayBytes(values,count)+'}');});}
void Tracker::scissor(VkCommandBuffer cb,unsigned first,unsigned count,const VkRect2D* values){LOCK;s.state(cb,[&](auto& c){
    if(count>16||c.state.dynamicHistory.size()>=128){c.state.incomplete=true;return;}c.state.dynamicHistory.push_back("{\"scissorFirst\":"+std::to_string(first)+",\"bytes\":"+arrayBytes(values,count)+'}');});}
void Tracker::beginPass(VkCommandBuffer cb,const VkRenderPassBeginInfo& info,VkSubpassContents contents){LOCK;s.state(cb,[&](auto& c){
    c.state.renderPass=id(info.renderPass);c.state.framebuffer=id(info.framebuffer);c.state.subpass=0;c.state.incomplete|=info.pNext!=nullptr;
    std::ostringstream o;o<<"{\"beginRenderPass\":"<<id(info.renderPass)<<",\"framebuffer\":"<<id(info.framebuffer)<<",\"contents\":"<<contents
        <<",\"area\":"<<hex(&info.renderArea,sizeof(info.renderArea))<<",\"clears\":"<<arrayBytes(info.pClearValues,info.clearValueCount)<<'}';c.state.barriers.push_back(o.str());});}
void Tracker::nextPass(VkCommandBuffer cb,VkSubpassContents contents){LOCK;s.state(cb,[&](auto& c){++c.state.subpass;c.state.barriers.push_back("{\"nextSubpassContents\":"+std::to_string(contents)+'}');});}
void Tracker::endPass(VkCommandBuffer cb){LOCK;s.state(cb,[&](auto& c){c.state.renderPass=0;c.state.framebuffer=0;});}
void Tracker::draw(VkCommandBuffer cb,unsigned count,unsigned instances,unsigned first,int vertexOffset,unsigned firstInstance,bool indexed){LOCK;
    s.state(cb,[&](auto& c){if(!s.cpuEvents)return;Impl::Event e;e.state=c.state;e.count=count;e.instances=instances;e.first=first;e.vertexOffset=vertexOffset;
        e.firstInstance=firstInstance;e.indexed=indexed;s.add(c,std::move(e));});}
void Tracker::indirect(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize off,unsigned count,unsigned stride,bool indexed){LOCK;
    s.state(cb,[&](auto& c){if(!s.cpuEvents)return;Impl::Event e;e.state=c.state;e.indirect=true;e.buffer=id(buffer);e.offset=off;e.count=count;e.stride=stride;e.indexed=indexed;s.add(c,e);});}
void Tracker::copy(VkCommandBuffer cb,VkBuffer source,VkBuffer destination,unsigned count,const VkBufferCopy* regions){LOCK;
    s.state(cb,[&](auto& c){for(unsigned i=0;regions&&i<count&&i<4096;++i){Impl::Event e;e.kind=Impl::Event::Copy;e.buffer=id(source);e.other=id(destination);e.region=regions[i];s.add(c,e);}});}
void Tracker::update(VkCommandBuffer cb,VkBuffer destination,VkDeviceSize off,VkDeviceSize bytes,const void* data){LOCK;
    s.state(cb,[&](auto& c){Impl::Event e;e.kind=Impl::Event::Update;e.other=id(destination);e.offset=off;
        if(data&&bytes<=65536){e.bytes.resize(bytes);std::memcpy(e.bytes.data(),data,bytes);s.add(c,std::move(e));}else c.state.incomplete=true;});}
void Tracker::barrier(VkCommandBuffer cb,VkPipelineStageFlags source,VkPipelineStageFlags destination,VkDependencyFlags flags,
    unsigned memoryCount,const VkMemoryBarrier* memory,unsigned bufferCount,const VkBufferMemoryBarrier* buffers,unsigned imageCount,const VkImageMemoryBarrier* images){LOCK;
    s.state(cb,[&](auto& c){if(c.state.barriers.size()>=128){c.state.incomplete=true;return;}std::ostringstream o;
        o<<"{\"sourceStages\":"<<source<<",\"destinationStages\":"<<destination<<",\"flags\":"<<flags
            <<",\"memoryBarriers\":"<<arrayBytes(memory,memoryCount)<<",\"bufferBarriers\":"<<arrayBytes(buffers,bufferCount)
            <<",\"imageBarriers\":"<<arrayBytes(images,imageCount)<<'}';c.state.barriers.push_back(o.str());});}
void Tracker::submit(VkQueue queue,unsigned count,const VkSubmitInfo* values,VkFence fence,bool success){LOCK;if(!s.cpuEvents)return;
    ++s.submission;for(unsigned i=0;values&&i<count&&i<128;++i){const auto& submit=values[i];std::ostringstream o;
        o<<"{\"submission\":"<<s.submission<<",\"queue\":"<<id(queue)<<",\"fence\":"<<id(fence)<<",\"success\":"<<success
            <<",\"waitSemaphores\":"<<arrayBytes(submit.pWaitSemaphores,submit.waitSemaphoreCount)
            <<",\"waitStages\":"<<arrayBytes(submit.pWaitDstStageMask,submit.waitSemaphoreCount)
            <<",\"signalSemaphores\":"<<arrayBytes(submit.pSignalSemaphores,submit.signalSemaphoreCount)
            <<",\"pNextUnresolved\":"<<(submit.pNext!=nullptr)<<'}';if(s.submissionLog.size()<128)s.submissionLog.push_back(o.str());else s.overflow=true;
        if(success)for(unsigned j=0;j<submit.commandBufferCount;++j){auto c=s.command(submit.pCommandBuffers[j]);s.walk(id(submit.pCommandBuffers[j]),c?c->recording:0,"null");}}
}
void Tracker::submit2(VkQueue queue,unsigned count,const VkSubmitInfo2* values,VkFence fence,bool success){LOCK;if(!s.cpuEvents)return;
    ++s.submission;for(unsigned i=0;values&&i<count&&i<128;++i){const auto& submit=values[i];std::ostringstream o;
        o<<"{\"submission\":"<<s.submission<<",\"queue\":"<<id(queue)<<",\"fence\":"<<id(fence)<<",\"success\":"<<success
            <<",\"waitInfos\":"<<arrayBytes(submit.pWaitSemaphoreInfos,submit.waitSemaphoreInfoCount)
            <<",\"signalInfos\":"<<arrayBytes(submit.pSignalSemaphoreInfos,submit.signalSemaphoreInfoCount)
            <<",\"pNextUnresolved\":"<<(submit.pNext!=nullptr)<<'}';if(s.submissionLog.size()<128)s.submissionLog.push_back(o.str());else s.overflow=true;
        if(success)for(unsigned j=0;j<submit.commandBufferInfoCount;++j){auto cb=submit.pCommandBufferInfos[j].commandBuffer;auto c=s.command(cb);s.walk(id(cb),c?c->recording:0,"null");}}
}
std::vector<Finding> Tracker::findings()const {LOCK;return s.results;}
std::string Tracker::report()const {LOCK;std::ostringstream o;
    o<<"{\"format\":\"KHARVOX_HUD_PROVENANCE_1\",\"overflow\":"<<s.overflow<<",\"cpuEmissions\":"<<s.cpuEvents
        <<",\"allocations\":"<<s.allocations<<",\"nativeAllocationsWithoutMappedAliasAtEmission\":"<<s.unmappedAllocations
        <<",\"drawsInspected\":"<<s.draws<<",\"replayImplemented\":false,\"nativeIdentificationCertified\":false"
        <<",\"limits\":[\"Arbitrary CPU memcpy and persistent mapped writes are not observed\",\"Compute/storage writes, bind-memory2 and copy2 are not certified\",\"Descriptor layouts, lifetime pinning, resource contents and extension dynamic states are incomplete\",\"Cross-queue ordering and completion are not certified\",\"Byte lineage is observed ownership, not proof of visible pixel contribution or replay safety\"]";
    auto strings=[&](const char* name,const std::vector<std::string>& list){o<<",\""<<name<<"\":[";for(size_t i=0;i<list.size();++i){if(i)o<<',';o<<list[i];}o<<']';};
    strings("cpuEvents",s.cpuLog);strings("submissions",s.submissionLog);o<<",\"draws\":[";
    for(size_t i=0;i<s.results.size();++i){if(i)o<<',';o<<s.results[i].details;}o<<"]}";return o.str();
}
#undef LOCK

Tracker* active() noexcept {static const bool enabled=[] {char text[8]{};return GetEnvironmentVariableA("KHARVOX_HUD_PROVENANCE",text,sizeof(text))==1&&text[0]=='1';}();
    if(!enabled)return nullptr;try {static Tracker tracker;return &tracker;}catch(...){return nullptr;}}
GuiScope::GuiScope(void* gui,std::uint64_t present,std::uint64_t level,bool root):tracker_(active()),previous_(currentSource){
    if(!tracker_||!root)return;try {source_=tracker_->beginGui(id(gui),present,level);currentSource=source_;}catch(...){currentSource={};}}
GuiScope::~GuiScope(){try {if(source_&&tracker_){_mm_sfence();tracker_->endGui(source_);}}catch(...){}currentSource=previous_;}
void nativeAllocation(void* gui,void* vertices,unsigned count,void* indices,unsigned indexCount,void* material){try {if(auto* t=active()){
    const auto source=currentSource.gui==id(gui)?currentSource:Source{};
    if(vertices&&count&&count<=131072)t->allocation(source,id(vertices),std::uint64_t(count)*48,id(material),id(indices),indexCount);}}catch(...){}}
void flushReport() noexcept {try {auto* t=active();if(!t)return;static unsigned writes{};if(writes>=4)return;
    auto report=t->report();if(report.find("\"drawsInspected\":0,")!=std::string::npos)return;
    wchar_t path[32768]{};const auto length=GetTempPathW(32768,path);if(!length||length>=32768)return;
    std::ofstream out(std::filesystem::path(path)/L"KHARVOX-HUD-PROVENANCE.jsonl",std::ios::app);out<<report<<'\n';++writes;
}catch(...) {}}
} // namespace kharvox::hudtrace
