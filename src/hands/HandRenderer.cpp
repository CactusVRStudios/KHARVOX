#include "HandRenderer.h"
#include "../hud/HudHook.h"
#include "../hud/OffhandHudPolicy.h"
#include "HandSceneDepthCopy.h"
#include "HandSceneDrawPolicy.h"
#include "HandCalibrationPolicy.h"
#include "WeaponSupportPolicy.h"
#include "WeaponSupportConfig.h"
#include "GrenadeVisualPolicy.h"
#include "../openxr/OpenXRBootstrap.h"
#include <windows.h>
#include <wincodec.h>
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <unordered_map>

namespace kharvox::hands {
namespace {

using namespace math;
struct Vertex {
    float position[3]{};
    float normal[3]{};
    float uv[2]{};
    float baseColor[4]{1.f,1.f,1.f,1.f};
    // x=metallic, y=roughness, z=normal texture, w=metal/rough texture.
    float material[4]{0.f,1.f,0.f,0.f};
    float morphPosition[3][3]{};
    float morphNormal[3][3]{};
};
struct DecodedImage { std::uint32_t width{},height{};std::vector<std::uint8_t> rgba; };

std::string trim(std::string value){const auto first=value.find_first_not_of(" \t\r\n");if(first==std::string::npos)return {};const auto last=value.find_last_not_of(" \t\r\n");return value.substr(first,last-first+1);}
bool readBytes(const std::filesystem::path&path,std::vector<std::uint8_t>&bytes){std::ifstream in(path,std::ios::binary);if(!in)return false;in.seekg(0,std::ios::end);const auto size=in.tellg();if(size<=0)return false;bytes.resize(static_cast<size_t>(size));in.seekg(0);return bool(in.read(reinterpret_cast<char*>(bytes.data()),size));}

bool decodePng(const void*data,size_t size,DecodedImage&decoded){
    if(!data||!size||size>UINT_MAX)return false;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);const bool uninitialize=SUCCEEDED(com);
    IWICImagingFactory*factory{};IWICStream*stream{};IWICBitmapDecoder*decoder{};IWICBitmapFrameDecode*frame{};IWICFormatConverter*converter{};
    bool ok=false;
    if(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))
        &&SUCCEEDED(factory->CreateStream(&stream))
        &&SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<void*>(data)),static_cast<DWORD>(size)))
        &&SUCCEEDED(factory->CreateDecoderFromStream(stream,nullptr,WICDecodeMetadataCacheOnLoad,&decoder))
        &&SUCCEEDED(decoder->GetFrame(0,&frame))
        &&SUCCEEDED(factory->CreateFormatConverter(&converter))
        &&SUCCEEDED(converter->Initialize(frame,GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0.f,WICBitmapPaletteTypeCustom))){
        UINT width{},height{};
        if(SUCCEEDED(converter->GetSize(&width,&height))&&width&&height&&uint64_t(width)*height<=268435456ull){
            decoded.width=width;decoded.height=height;decoded.rgba.resize(size_t(width)*height*4);
            ok=SUCCEEDED(converter->CopyPixels(nullptr,width*4,static_cast<UINT>(decoded.rgba.size()),decoded.rgba.data()));
        }
    }
    if(converter)converter->Release();if(frame)frame->Release();if(decoder)decoder->Release();if(stream)stream->Release();if(factory)factory->Release();if(uninitialize)CoUninitialize();return ok;
}

const cgltf_accessor* attribute(const cgltf_primitive&primitive,cgltf_attribute_type type){for(cgltf_size i=0;i<primitive.attributes_count;i++)if(primitive.attributes[i].type==type&&primitive.attributes[i].index==0)return primitive.attributes[i].data;return nullptr;}
void transformPosition(const float m[16],float v[3]){const float x=v[0],y=v[1],z=v[2];v[0]=m[0]*x+m[4]*y+m[8]*z+m[12];v[1]=m[1]*x+m[5]*y+m[9]*z+m[13];v[2]=m[2]*x+m[6]*y+m[10]*z+m[14];}
void transformNormal(const float m[16],float v[3]){const float x=v[0],y=v[1],z=v[2];v[0]=m[0]*x+m[4]*y+m[8]*z;v[1]=m[1]*x+m[5]*y+m[9]*z;v[2]=m[2]*x+m[6]*y+m[10]*z;const float length=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(length>1e-6f){v[0]/=length;v[1]/=length;v[2]/=length;}}
bool nodeInScene(const cgltf_node*node,const cgltf_scene*scene){if(!scene)return true;const cgltf_node*root=node;while(root&&root->parent)root=root->parent;for(cgltf_size i=0;i<scene->nodes_count;i++)if(scene->nodes[i]==root)return true;return false;}
bool embeddedTextureBytes(const cgltf_texture_view&view,const void*&bytes,size_t&size){bytes=nullptr;size=0;if(!view.texture||!view.texture->image)return false;const auto*bufferView=view.texture->image->buffer_view;if(!bufferView||!bufferView->buffer||!bufferView->buffer->data)return false;bytes=static_cast<const std::uint8_t*>(bufferView->buffer->data)+bufferView->offset;size=bufferView->size;return true;}

} // namespace

struct HandRenderer::Impl {
    struct Texture {VkImage image{};VkDeviceMemory memory{};VkImageView view{};std::uint64_t hash{};std::uint32_t width{},height{};bool srgb{};};
    struct DepthTarget {VkImage image{};VkDeviceMemory memory{};VkImageView view{};};
    struct Asset {VkBuffer vertices{};VkDeviceMemory vertexMemory{};VkBuffer indices{};VkDeviceMemory indexMemory{};std::uint32_t indexCount{};std::uint64_t sourceHash{};std::array<std::shared_ptr<Texture>,3> textures{};VkDescriptorSet descriptor{};bool ready{};std::string filename;std::array<float,3> minimum{},maximum{};std::array<std::array<float,6>,8> hudSections{};unsigned hudSectionMask{};};
    std::vector<std::weak_ptr<Texture>> textureCache;
    VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};std::uint32_t queueFamily{};KharvoxVulkanDispatch vk{};VkFormat format{};HandLog log;
    struct ScenePipeline {VkFormat colorFormat{VK_FORMAT_UNDEFINED};VkFormat depthFormat{VK_FORMAT_UNDEFINED};bool reverseDepth{};VkRenderPass renderPass{};VkPipeline pipeline{};};
    VkCommandPool uploadPool{};VkCommandBuffer uploadCommand{};VkSampler sampler{};VkDescriptorSetLayout descriptorLayout{};VkDescriptorPool descriptorPool{};VkPipelineLayout pipelineLayout{};VkPipeline pipeline{};VkRenderPass renderPass{};VkShaderModule vertexShader{},fragmentShader{};
    std::vector<ScenePipeline> scenePipelines;std::vector<VkFramebuffer> sceneFramebuffers;
    struct SceneDepth {DepthTarget target;VkExtent2D extent{};VkFormat format{};bool initialized{};};
    std::vector<SceneDepth> sceneDepthCopies;size_t sceneDepthCopiesUsed{};
    std::array<std::vector<VkImageView>,2> eyeViews{};std::array<std::vector<DepthTarget>,2> depthTargets{};std::array<std::vector<VkFramebuffer>,2> framebuffers{};std::array<VkExtent2D,2> extents{};
    Asset laserAsset{};
    Asset grenadeAsset{};
    Asset grenadeVisualHand{},grenadeVisualAsset{};
    bool grenadeVisualReady{};
    std::array<Asset,4> assets{};std::array<bool,4> mirrorX{};HandCalibration leftCalibration{},rightCalibration{};
    std::array<std::array<Asset,2>,static_cast<size_t>(KharvoxWeaponKind::Count)> supportAssets{};
    std::array<WeaponSupportProfile,static_cast<size_t>(KharvoxWeaponKind::Count)> supportProfiles{};
    std::array<Asset,static_cast<size_t>(KharvoxWeaponKind::Count)> primaryAssets{};
    std::array<WeaponSupportProfile,static_cast<size_t>(KharvoxWeaponKind::Count)> primaryProfiles{};
    uint32_t reviewedPrimaryLogged{};
    uint32_t reviewedSupportLogged{};
    struct FingerData {FingerMorphHeader header{};bool loaded{};};
    std::array<FingerData,2> fingerData{};
    std::array<std::array<HandCalibration,2>,
        static_cast<size_t>(KharvoxWeaponKind::Count)> weaponCalibrations{}, defaultWeaponCalibrations{};
    std::filesystem::path root;bool initialized{};
    HandCalibration defaultLeftCalibration{},defaultRightCalibration{};
    CalibrationMode calibrationMode{CalibrationMode::None};bool calibrateLeft{true};
    bool calibrationPlusWasDown{},calibrationHandWasDown{},calibrationResetWasDown{};
    KharvoxWeaponKind calibrationWeapon{KharvoxWeaponKind::Unknown};
    bool calibrationLeftHanded{},calibrationContextInitialized{};
    ULONGLONG lastCalibrationStep{};

    void say(const std::string&message)const{if(log)log("[HANDS] "+message);}
    std::uint32_t memoryType(std::uint32_t bits,VkMemoryPropertyFlags required)const{VkPhysicalDeviceMemoryProperties p{};vk.getPhysicalDeviceMemoryProperties(physical,&p);for(std::uint32_t i=0;i<p.memoryTypeCount;i++)if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&required)==required)return i;return UINT32_MAX;}
    bool buffer(VkDeviceSize size,VkBufferUsageFlags usage,const void*source,VkBuffer&out,VkDeviceMemory&memory){VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=size;bi.usage=usage;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;if(vk.createBuffer(device,&bi,nullptr,&out)!=VK_SUCCESS)return false;VkMemoryRequirements req{};vk.getBufferMemoryRequirements(device,out,&req);const auto type=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);if(type==UINT32_MAX){vk.destroyBuffer(device,out,nullptr);out=VK_NULL_HANDLE;return false;}VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;if(vk.allocateMemory(device,&ai,nullptr,&memory)!=VK_SUCCESS||vk.bindBufferMemory(device,out,memory,0)!=VK_SUCCESS){if(memory)vk.freeMemory(device,memory,nullptr);vk.destroyBuffer(device,out,nullptr);memory=VK_NULL_HANDLE;out=VK_NULL_HANDLE;return false;}void*mapped{};if(vk.mapMemory(device,memory,0,size,0,&mapped)!=VK_SUCCESS){vk.freeMemory(device,memory,nullptr);vk.destroyBuffer(device,out,nullptr);memory=VK_NULL_HANDLE;out=VK_NULL_HANDLE;return false;}std::memcpy(mapped,source,static_cast<size_t>(size));vk.unmapMemory(device,memory);return true;}
    bool texture(const DecodedImage&decoded,std::shared_ptr<Texture>&out,bool srgb);
    bool depthTarget(VkExtent2D extent,DepthTarget&out,VkFormat depthFormat=VK_FORMAT_D32_SFLOAT,bool transfer=false);
    void destroyDepth(DepthTarget& target){
        if(target.view)vk.destroyImageView(device,target.view,nullptr);
        if(target.image)vk.destroyImage(device,target.image,nullptr);
        if(target.memory)vk.freeMemory(device,target.memory,nullptr);
        target={};
    }
    bool loadAsset(const std::filesystem::path&path,Asset&asset,const Asset* materialSource=nullptr);
    void publishSupportHudTemplates();
    void loadFingerMorphs(const std::filesystem::path&path,Asset&asset,std::vector<Vertex>&vertices);
    bool createCommonResources();
    bool createGrenadeAsset();
    bool createGraphicsPipeline(VkRenderPass compatibleRenderPass,
        VkCompareOp depthCompare,bool encodeSrgb,VkPipeline&out);
    ScenePipeline* scenePipeline(const HandSceneTarget&target);
    void drawGeometry(VkCommandBuffer commandBuffer,VkPipeline selectedPipeline,
        VkExtent2D extent,const HandEyeView&view,const HandPose&leftGrip,
        const HandPose&rightGrip,const HandVisibilityOutput&visibility,
        const HandGameplayState&gameplay,const HandPose&laser = {});
    void destroyAsset(Asset&asset);
    HandCalibration& selectedCalibration();
    const HandCalibration& selectedCalibration()const;
    const HandCalibration& renderCalibration(bool left,HandModelKind kind,
        KharvoxWeaponKind weapon,bool leftHanded)const;
    void loadCalibrationFile(const wchar_t* filename, bool defaults);
    void saveCalibration();
    void writeCalibrationStatus(const char*message=nullptr)const;
    void pollCalibration(const HandGameplayState&gameplay);
};

void HandRenderer::Impl::loadFingerMorphs(const std::filesystem::path&path,
    Asset&asset,std::vector<Vertex>&vertices){
    std::vector<std::uint8_t> raw;
    if(!readBytes(path,raw))return;
    asset.sourceHash=handAssetHash(raw.data(),raw.size());
    const bool left=asset.filename=="DOOM_LEFT_HAND_FIST.glb";
    if(!left&&asset.filename!="DOOM_RIGHT_HAND_FIST.glb")return;
    std::vector<std::uint8_t> bytes;
    const auto file=path.parent_path()/(left?"DOOM_LEFT_HAND_FINGERS.bin":"DOOM_RIGHT_HAND_FINGERS.bin");
    FingerMorphHeader header{};
    if(!readBytes(file,bytes)||!readFingerMorphHeader(bytes.data(),bytes.size(),
            vertices.size(),asset.sourceHash,header)){
        say(asset.filename+" finger data absent/mismatched; retaining static pose");return;
    }
    const size_t values=vertices.size()*18;
    for(size_t i=0;i<values;++i){float value{};std::memcpy(&value,bytes.data()+fingerMorphHeaderBytes+i*4,4);
        if(!std::isfinite(value)||std::abs(value)>10.f){say("invalid finger morph; retaining static pose");return;}}
    for(size_t shape=0;shape<3;++shape)for(size_t i=0;i<vertices.size();++i){
        const auto offset=fingerMorphHeaderBytes+(shape*vertices.size()+i)*6*sizeof(float);
        std::memcpy(vertices[i].morphPosition[shape],bytes.data()+offset,12);
        std::memcpy(vertices[i].morphNormal[shape],bytes.data()+offset+12,12);
    }
    fingerData[left?0:1]={header,true};
    say(std::string(left?"left":"right")+" authored index/grip/thumb morphs loaded; wrist/grasp frames matched to GLB hash");
}

bool HandRenderer::Impl::texture(const DecodedImage&decoded,std::shared_ptr<Texture>&shared,bool srgb){
    if(!decoded.width||!decoded.height||decoded.rgba.empty())return false;
    const auto hash=handAssetHash(decoded.rgba.data(),decoded.rgba.size());
    for(const auto&entry:textureCache)if(auto cached=entry.lock()){
        if(cached->hash==hash&&cached->width==decoded.width&&cached->height==decoded.height&&cached->srgb==srgb){
            shared=std::move(cached);say("shared hand texture reused "+std::to_string(decoded.width)+"x"+std::to_string(decoded.height));return true;
        }
    }
    shared=std::make_shared<Texture>();auto&out=*shared;
    out.hash=hash;out.width=decoded.width;out.height=decoded.height;out.srgb=srgb;
    VkBuffer staging{};VkDeviceMemory stagingMemory{};
    if(!buffer(decoded.rgba.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            decoded.rgba.data(),staging,stagingMemory))return false;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType=VK_IMAGE_TYPE_2D;imageInfo.format=srgb
        ?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent={decoded.width,decoded.height,1};imageInfo.mipLevels=1;
    imageInfo.arrayLayers=1;imageInfo.samples=VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling=VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    bool ok=vk.createImage(device,&imageInfo,nullptr,&out.image)==VK_SUCCESS;
    VkMemoryRequirements requirements{};
    if(ok){vk.getImageMemoryRequirements(device,out.image,&requirements);const auto type=memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);if(type==UINT32_MAX)ok=false;else{VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=type;ok=vk.allocateMemory(device,&allocation,nullptr,&out.memory)==VK_SUCCESS&&vk.bindImageMemory(device,out.image,out.memory,0)==VK_SUCCESS;}}
    if(ok){
        vk.resetCommandBuffer(uploadCommand,0);VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;ok=vk.beginCommandBuffer(uploadCommand,&begin)==VK_SUCCESS;
        if(ok){VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.image=out.image;barrier.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;barrier.subresourceRange.levelCount=1;barrier.subresourceRange.layerCount=1;vk.cmdPipelineBarrier(uploadCommand,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);VkBufferImageCopy copy{};copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;copy.imageSubresource.layerCount=1;copy.imageExtent={decoded.width,decoded.height,1};vk.cmdCopyBufferToImage(uploadCommand,staging,out.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;vk.cmdPipelineBarrier(uploadCommand,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);ok=vk.endCommandBuffer(uploadCommand)==VK_SUCCESS;}
        if(ok){VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&uploadCommand;ok=vk.queueSubmit(queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS&&vk.queueWaitIdle(queue)==VK_SUCCESS;}
    }
    if(staging)vk.destroyBuffer(device,staging,nullptr);if(stagingMemory)vk.freeMemory(device,stagingMemory,nullptr);
    if(ok){VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=out.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=srgb?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM;vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;vi.subresourceRange.levelCount=1;vi.subresourceRange.layerCount=1;ok=vk.createImageView(device,&vi,nullptr,&out.view)==VK_SUCCESS;}
    if(ok)textureCache.push_back(shared);
    return ok;
}

bool HandRenderer::Impl::createGrenadeAsset(){
    // Original low-poly preview, not a copied game asset. Native equipment
    // remains responsible for spawning and simulating the released projectile.
    constexpr float profile[][2]{{-.052f,0},{-.046f,.018f},{-.034f,.027f},
        {-.012f,.028f},{-.006f,.028f},{.021f,.028f},{.036f,.022f},
        {.041f,.012f},{.052f,.012f},{.056f,0}};
    constexpr unsigned sides=16,rings=sizeof(profile)/sizeof(profile[0]);
    std::vector<Vertex>vertices;std::vector<std::uint32_t>indices;
    for(unsigned ring=0;ring<rings;++ring)for(unsigned side=0;side<sides;++side){
        const float angle=side*6.28318530718f/sides,c=std::cos(angle),s=std::sin(angle);
        const auto before=ring?ring-1:ring,after=std::min(ring+1,rings-1);
        const float dy=profile[after][0]-profile[before][0];
        const float dr=profile[after][1]-profile[before][1];
        Vertex v{};v.position[0]=profile[ring][1]*c;v.position[1]=profile[ring][0];v.position[2]=profile[ring][1]*s;
        v.normal[0]=dy*c;v.normal[1]=-dr;v.normal[2]=dy*s;
        const float length=std::sqrt(dy*dy+dr*dr);for(float&n:v.normal)n/=length;
        const bool band=ring==3||ring==4,cap=ring>=7;
        const float shade=side%4==0?.7f:1.f;
        v.baseColor[0]=(band?.8f:cap?.24f:.12f)*shade;
        v.baseColor[1]=(band?.22f:cap?.24f:.17f)*shade;
        v.baseColor[2]=(band?.025f:cap?.25f:.12f)*shade;
        v.material[0]=.45f;v.material[1]=.55f;v.material[3]=-1.f;
        vertices.push_back(v);
    }
    for(unsigned ring=0;ring+1<rings;++ring)for(unsigned side=0;side<sides;++side){
        const auto a=ring*sides+side,b=ring*sides+(side+1)%sides;
        for(auto i:{a,b,a+sides,b,b+sides,a+sides})indices.push_back(i);
    }
    grenadeAsset.ready=buffer(vertices.size()*sizeof(Vertex),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        vertices.data(),grenadeAsset.vertices,grenadeAsset.vertexMemory)
        &&buffer(indices.size()*sizeof(std::uint32_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            indices.data(),grenadeAsset.indices,grenadeAsset.indexMemory);
    grenadeAsset.indexCount=static_cast<std::uint32_t>(indices.size());
    return grenadeAsset.ready;
}

bool HandRenderer::Impl::depthTarget(VkExtent2D extent,DepthTarget&out,VkFormat depthFormat,bool transfer){
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType=VK_IMAGE_TYPE_2D;imageInfo.format=depthFormat;
    imageInfo.extent={extent.width,extent.height,1};imageInfo.mipLevels=1;
    imageInfo.arrayLayers=1;imageInfo.samples=VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling=VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if(transfer)imageInfo.usage|=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(vk.createImage(device,&imageInfo,nullptr,&out.image)!=VK_SUCCESS)return false;
    VkMemoryRequirements requirements{};vk.getImageMemoryRequirements(device,out.image,&requirements);
    const auto type=memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(type==UINT32_MAX)return false;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=type;
    if(vk.allocateMemory(device,&allocation,nullptr,&out.memory)!=VK_SUCCESS
        ||vk.bindImageMemory(device,out.image,out.memory,0)!=VK_SUCCESS)return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image=out.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;
    view.format=depthFormat;
    view.subresourceRange.aspectMask=handSceneDepthAspect(depthFormat);
    view.subresourceRange.levelCount=1;view.subresourceRange.layerCount=1;
    return vk.createImageView(device,&view,nullptr,&out.view)==VK_SUCCESS;
}

bool HandRenderer::Impl::loadAsset(const std::filesystem::path&path,Asset&asset,const Asset* materialSource){
    asset.filename=path.filename().u8string();cgltf_options options{};cgltf_data*data{};
    const auto utf8=path.u8string();if(cgltf_parse_file(&options,utf8.c_str(),&data)!=cgltf_result_success){say("cannot parse "+asset.filename+"; model disabled");return false;}
    const auto freeData=[&]{cgltf_free(data);};
    if(cgltf_load_buffers(&options,data,utf8.c_str())!=cgltf_result_success){say("cannot load GLB buffers for "+asset.filename+"; model disabled");freeData();return false;}
    // The supplied files contain two posed scenes in one GLB. Respect the
    // file's default scene instead of blindly selecting the first mesh node;
    // the Fist and Gun files intentionally select different scenes.
    const cgltf_node*node{};const cgltf_primitive*primitive{};
    for(cgltf_size n=0;n<data->nodes_count&&!primitive;n++)
        if(data->nodes[n].mesh&&data->nodes[n].mesh->primitives_count
            &&nodeInScene(&data->nodes[n],data->scene)){
            node=&data->nodes[n];primitive=&data->nodes[n].mesh->primitives[0];
        }
    if(!primitive||primitive->type!=cgltf_primitive_type_triangles||!primitive->indices){say(asset.filename+" must contain one indexed triangle primitive");freeData();return false;}
    const auto*positions=attribute(*primitive,cgltf_attribute_type_position);const auto*normals=attribute(*primitive,cgltf_attribute_type_normal);const auto*uvs=attribute(*primitive,cgltf_attribute_type_texcoord);const auto*joints=attribute(*primitive,cgltf_attribute_type_joints);const auto*weights=attribute(*primitive,cgltf_attribute_type_weights);
    if(!positions||!normals||!uvs||positions->count!=normals->count||positions->count!=uvs->count){say(asset.filename+" requires POSITION, NORMAL and TEXCOORD_0");freeData();return false;}
    cgltf_float nodeTransform[16];cgltf_node_transform_world(node,nodeTransform);
    const bool skinned=node->skin&&joints&&weights
        &&joints->count==positions->count&&weights->count==positions->count
        &&node->skin->inverse_bind_matrices
        &&node->skin->inverse_bind_matrices->count>=node->skin->joints_count;
    std::vector<Mat4>jointTransforms;
    if(skinned){
        jointTransforms.resize(node->skin->joints_count);
        for(cgltf_size joint=0;joint<node->skin->joints_count;joint++){
            cgltf_float world[16]{};cgltf_float inverseBind[16]{};
            cgltf_node_transform_world(node->skin->joints[joint],world);
            if(!cgltf_accessor_read_float(node->skin->inverse_bind_matrices,
                    joint,inverseBind,16)){
                say(asset.filename+" has unreadable inverse bind matrices");
                freeData();return false;
            }
            Mat4 worldMatrix{},inverseBindMatrix{};
            std::copy_n(world,16,worldMatrix.m);
            std::copy_n(inverseBind,16,inverseBindMatrix.m);
            jointTransforms[joint]=multiply(worldMatrix,inverseBindMatrix);
        }
    }
    const auto*material=primitive->material;float baseFactor[4]{1,1,1,1};float metallic=1.f,roughness=1.f;bool hasNormal=false,hasMr=false;
    if(material&&material->has_pbr_metallic_roughness){for(int i=0;i<4;i++)baseFactor[i]=material->pbr_metallic_roughness.base_color_factor[i];metallic=material->pbr_metallic_roughness.metallic_factor;roughness=material->pbr_metallic_roughness.roughness_factor;hasNormal=material->normal_texture.texture!=nullptr;hasMr=material->pbr_metallic_roughness.metallic_roughness_texture.texture!=nullptr;}
    // Controller hands are opaque geometry. Blender's exported base-color PNG
    // may retain unused transparent texels even though glTF alphaMode is
    // OPAQUE; letting those texels reach our blend pipeline made valid surface
    // fragments look partially transparent.
    baseFactor[3]=1.f;
    std::vector<Vertex>vertices(positions->count);for(cgltf_size i=0;i<positions->count;i++){auto&v=vertices[i];cgltf_accessor_read_float(positions,i,v.position,3);cgltf_accessor_read_float(normals,i,v.normal,3);cgltf_accessor_read_float(uvs,i,v.uv,2);if(skinned){cgltf_uint jointIndices[4]{};cgltf_float jointWeights[4]{};if(!cgltf_accessor_read_uint(joints,i,jointIndices,4)||!cgltf_accessor_read_float(weights,i,jointWeights,4)){say(asset.filename+" has unreadable skin weights");freeData();return false;}const std::array<float,3>sourcePosition{{v.position[0],v.position[1],v.position[2]}};const std::array<float,3>sourceNormal{{v.normal[0],v.normal[1],v.normal[2]}};v.position[0]=v.position[1]=v.position[2]=0.f;v.normal[0]=v.normal[1]=v.normal[2]=0.f;float totalWeight=0.f;for(size_t influence=0;influence<4;influence++){const float weight=jointWeights[influence];const auto joint=jointIndices[influence];if(weight<=0.f||joint>=jointTransforms.size())continue;float posedPosition[3]{sourcePosition[0],sourcePosition[1],sourcePosition[2]};float posedNormal[3]{sourceNormal[0],sourceNormal[1],sourceNormal[2]};transformPosition(jointTransforms[joint].m,posedPosition);transformNormal(jointTransforms[joint].m,posedNormal);for(int component=0;component<3;component++){v.position[component]+=posedPosition[component]*weight;v.normal[component]+=posedNormal[component]*weight;}totalWeight+=weight;}if(totalWeight<=.0001f){v.position[0]=sourcePosition[0];v.position[1]=sourcePosition[1];v.position[2]=sourcePosition[2];v.normal[0]=sourceNormal[0];v.normal[1]=sourceNormal[1];v.normal[2]=sourceNormal[2];transformPosition(nodeTransform,v.position);transformNormal(nodeTransform,v.normal);}else{const float inverseWeight=1.f/totalWeight;for(int component=0;component<3;component++){v.position[component]*=inverseWeight;v.normal[component]*=inverseWeight;}const float normalLength=std::sqrt(v.normal[0]*v.normal[0]+v.normal[1]*v.normal[1]+v.normal[2]*v.normal[2]);if(normalLength>.000001f){v.normal[0]/=normalLength;v.normal[1]/=normalLength;v.normal[2]/=normalLength;}}}else{transformPosition(nodeTransform,v.position);transformNormal(nodeTransform,v.normal);}for(int j=0;j<4;j++)v.baseColor[j]=baseFactor[j];v.material[0]=metallic;v.material[1]=roughness;v.material[2]=hasNormal?1.f:0.f;v.material[3]=hasMr?1.f:0.f;}
    std::vector<std::uint32_t>indices(primitive->indices->count);for(cgltf_size i=0;i<primitive->indices->count;i++)indices[i]=static_cast<std::uint32_t>(cgltf_accessor_read_index(primitive->indices,i));
    loadFingerMorphs(path,asset,vertices);
    if(!vertices.empty()){
        std::copy_n(vertices.front().position,3,asset.minimum.begin());asset.maximum=asset.minimum;
        std::vector<std::array<float,6>> ranges(vertices.size());
        for(size_t v=0;v<vertices.size();++v)for(int i=0;i<3;++i){
            const auto& vertex=vertices[v];
            float minimum=vertex.position[i],maximum=minimum;
            for(int shape=0;shape<3;++shape){
                minimum+=std::min(0.f,vertex.morphPosition[shape][i]);
                maximum+=std::max(0.f,vertex.morphPosition[shape][i]);
            }
            asset.minimum[i]=std::min(asset.minimum[i],minimum);
            asset.maximum[i]=std::max(asset.maximum[i],maximum);
            ranges[v][i]=minimum;ranges[v][3+i]=maximum;
        }
        int longest=0;for(int i=1;i<3;++i)
            if(asset.maximum[i]-asset.minimum[i]>asset.maximum[longest]-asset.minimum[longest])longest=i;
        const float span=asset.maximum[longest]-asset.minimum[longest];
        // Partition triangle envelopes, not individual vertices: long forearm
        // triangles and every finger-morph state remain conservatively covered.
        if(span>1e-6f)for(size_t t=0;t+2<indices.size();t+=3){
            std::array<float,6> triangle{};
            for(int corner=0;corner<3;++corner){
                if(indices[t+corner]>=ranges.size()){freeData();return false;}
                const auto& range=ranges[indices[t+corner]];
                for(int i=0;i<3;++i){
                    triangle[i]=corner?std::min(triangle[i],range[i]):range[i];
                    triangle[3+i]=corner?std::max(triangle[3+i],range[3+i]):range[3+i];
                }
            }
            for(unsigned section=0;section<8;++section){
                const float lo=asset.minimum[longest]+span*section/8.f;
                const float hi=asset.minimum[longest]+span*(section+1)/8.f;
                if(triangle[longest]>hi||triangle[3+longest]<lo)continue;
                auto part=triangle;part[longest]=std::max(part[longest],lo);
                part[3+longest]=std::min(part[3+longest],hi);
                auto& target=asset.hudSections[section];const unsigned bit=1u<<section;
                if(!(asset.hudSectionMask&bit))target=part;
                else for(int i=0;i<3;++i){target[i]=std::min(target[i],part[i]);target[3+i]=std::max(target[3+i],part[3+i]);}
                asset.hudSectionMask|=bit;
            }
        }
    }
    bool ok=buffer(vertices.size()*sizeof(Vertex),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,vertices.data(),asset.vertices,asset.vertexMemory)&&buffer(indices.size()*sizeof(std::uint32_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT,indices.data(),asset.indices,asset.indexMemory);asset.indexCount=static_cast<std::uint32_t>(indices.size());
    std::array<DecodedImage,3>decoded{};decoded[0]={1,1,{255,255,255,255}};decoded[1]={1,1,{128,128,255,255}};decoded[2]={1,1,{255,255,255,255}};
    auto decodeView=[&](const cgltf_texture_view&view,DecodedImage&target,const char*label){const void*bytes{};size_t size{};if(!embeddedTextureBytes(view,bytes,size)||!decodePng(bytes,size,target)){say(asset.filename+" "+label+" texture unavailable; using neutral fallback");return false;}return true;};
    if(!materialSource&&material&&material->has_pbr_metallic_roughness){decodeView(material->pbr_metallic_roughness.base_color_texture,decoded[0],"base-color");if(hasMr)decodeView(material->pbr_metallic_roughness.metallic_roughness_texture,decoded[2],"metallic/roughness");if(hasNormal)decodeView(material->normal_texture,decoded[1],"normal");}
    for(size_t alpha=3;alpha<decoded[0].rgba.size();alpha+=4)
        decoded[0].rgba[alpha]=255;
    if(materialSource&&materialSource->ready)asset.textures=materialSource->textures;
    else for(size_t i=0;i<asset.textures.size()&&ok;i++)
        ok=texture(decoded[i],asset.textures[i],i==0);
    if(ok){VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocation.descriptorPool=descriptorPool;allocation.descriptorSetCount=1;allocation.pSetLayouts=&descriptorLayout;ok=vk.allocateDescriptorSets(device,&allocation,&asset.descriptor)==VK_SUCCESS;}
    if(ok){std::array<VkDescriptorImageInfo,3>infos{};std::array<VkWriteDescriptorSet,3>writes{};for(std::uint32_t i=0;i<3;i++){infos[i].sampler=sampler;infos[i].imageView=asset.textures[i]->view;infos[i].imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=asset.descriptor;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&infos[i];}vk.updateDescriptorSets(device,3,writes.data(),0,nullptr);}
    freeData();asset.ready=ok;if(ok)say("loaded "+asset.filename+" vertices="+std::to_string(vertices.size())+" indices="+std::to_string(indices.size())+(skinned?" static-pose=baked":"")+" PBR=base-color"+(hasNormal?"+normal":"")+(hasMr?"+metallic-roughness":""));else say("GPU resources failed for "+asset.filename+"; model disabled");return ok;
}

bool HandRenderer::Impl::createGraphicsPipeline(
    VkRenderPass compatibleRenderPass,VkCompareOp depthCompare,bool encodeSrgb,
    VkPipeline&out){
    if(!vertexShader||!fragmentShader||!pipelineLayout||!compatibleRenderPass)
        return false;
    std::array<VkPipelineShaderStageCreateInfo,2>stages{};
    stages[0]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertexShader;
    stages[0].pName="main";
    stages[1]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragmentShader;
    stages[1].pName="main";
    const std::uint32_t outputTransfer=encodeSrgb?1u:0u;
    const VkSpecializationMapEntry transferEntry{0,0,sizeof(outputTransfer)};
    const VkSpecializationInfo transferInfo{1,&transferEntry,
        sizeof(outputTransfer),&outputTransfer};
    stages[1].pSpecializationInfo=&transferInfo;
    VkVertexInputBindingDescription vertexBinding{0,sizeof(Vertex),VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription,11>attributes{{
        {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,position)},
        {1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,normal)},
        {2,0,VK_FORMAT_R32G32_SFLOAT,offsetof(Vertex,uv)},
        {3,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(Vertex,baseColor)},
        {4,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(Vertex,material)},
        {5,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,morphPosition)},
        {6,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,morphPosition)+12},
        {7,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,morphPosition)+24},
        {8,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,morphNormal)},
        {9,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,morphNormal)+12},
        {10,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,morphNormal)+24}}};
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount=1;vertexInput.pVertexBindingDescriptions=&vertexBinding;
    vertexInput.vertexAttributeDescriptionCount=static_cast<std::uint32_t>(attributes.size());vertexInput.pVertexAttributeDescriptions=attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=1;viewport.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1.f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};multisample.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};depthState.depthTestEnable=VK_TRUE;depthState.depthWriteEnable=VK_TRUE;depthState.depthCompareOp=depthCompare;
    VkPipelineColorBlendAttachmentState blend{};blend.blendEnable=VK_FALSE;blend.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blendState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blendState.attachmentCount=1;blendState.pAttachments=&blend;
    std::array<VkDynamicState,2>dynamicStates{{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR}};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=static_cast<std::uint32_t>(dynamicStates.size());dynamic.pDynamicStates=dynamicStates.data();
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.stageCount=static_cast<std::uint32_t>(stages.size());info.pStages=stages.data();info.pVertexInputState=&vertexInput;info.pInputAssemblyState=&assembly;info.pViewportState=&viewport;info.pRasterizationState=&raster;info.pMultisampleState=&multisample;info.pDepthStencilState=&depthState;info.pColorBlendState=&blendState;info.pDynamicState=&dynamic;info.layout=pipelineLayout;info.renderPass=compatibleRenderPass;
    return vk.createGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&out)==VK_SUCCESS;
}

bool HandRenderer::Impl::createCommonResources(){
    if(!vk.createCommandPool||!vk.allocateCommandBuffers||!vk.createBuffer
        ||!vk.getBufferMemoryRequirements||!vk.bindBufferMemory||!vk.mapMemory
        ||!vk.unmapMemory||!vk.cmdCopyBufferToImage||!vk.createRenderPass
        ||!vk.createGraphicsPipelines||!vk.cmdBeginRenderPass
        ||!vk.cmdDrawIndexed){say("required Vulkan functions unavailable; renderer disabled");return false;}
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};poolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;poolInfo.queueFamilyIndex=queueFamily;if(vk.createCommandPool(device,&poolInfo,nullptr,&uploadPool)!=VK_SUCCESS)return false;
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};commandInfo.commandPool=uploadPool;commandInfo.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;commandInfo.commandBufferCount=1;if(vk.allocateCommandBuffers(device,&commandInfo,&uploadCommand)!=VK_SUCCESS)return false;
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};samplerInfo.magFilter=VK_FILTER_LINEAR;samplerInfo.minFilter=VK_FILTER_LINEAR;samplerInfo.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;samplerInfo.addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT;samplerInfo.addressModeV=VK_SAMPLER_ADDRESS_MODE_REPEAT;samplerInfo.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;samplerInfo.maxLod=0.f;if(vk.createSampler(device,&samplerInfo,nullptr,&sampler)!=VK_SUCCESS)return false;
    std::array<VkDescriptorSetLayoutBinding,3>bindings{};for(std::uint32_t i=0;i<bindings.size();i++){bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;}VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layoutInfo.bindingCount=static_cast<std::uint32_t>(bindings.size());layoutInfo.pBindings=bindings.data();if(vk.createDescriptorSetLayout(device,&layoutInfo,nullptr,&descriptorLayout)!=VK_SUCCESS)return false;
    constexpr uint32_t handDescriptorSets=6+2*static_cast<uint32_t>(KharvoxWeaponKind::Count);
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3*handDescriptorSets};VkDescriptorPoolCreateInfo descriptorPoolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};descriptorPoolInfo.maxSets=handDescriptorSets;descriptorPoolInfo.poolSizeCount=1;descriptorPoolInfo.pPoolSizes=&poolSize;if(vk.createDescriptorPool(device,&descriptorPoolInfo,nullptr,&descriptorPool)!=VK_SUCCESS)return false;
    VkAttachmentDescription color{};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentDescription depth{};
    depth.format=VK_FORMAT_D32_SFLOAT;
    depth.samples=VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorReference{
        0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthReference{
        1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    subpass.pDepthStencilAttachment = &depthReference;
    VkRenderPassCreateInfo renderPassInfo{
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    const std::array<VkAttachmentDescription,2> attachments{{color,depth}};
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    if (vk.createRenderPass(device, &renderPassInfo, nullptr, &renderPass) !=
        VK_SUCCESS)
      return false;
    std::vector<std::uint8_t>vertexSpv,fragmentSpv;if(!readBytes(root/L"HandPbr.vert.spv",vertexSpv)||!readBytes(root/L"HandPbr.frag.spv",fragmentSpv)||vertexSpv.size()%4||fragmentSpv.size()%4){say("hand shader binaries missing or invalid; renderer disabled");return false;}VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shaderInfo.codeSize=vertexSpv.size();shaderInfo.pCode=reinterpret_cast<const std::uint32_t*>(vertexSpv.data());if(vk.createShaderModule(device,&shaderInfo,nullptr,&vertexShader)!=VK_SUCCESS)return false;shaderInfo.codeSize=fragmentSpv.size();shaderInfo.pCode=reinterpret_cast<const std::uint32_t*>(fragmentSpv.data());if(vk.createShaderModule(device,&shaderInfo,nullptr,&fragmentShader)!=VK_SUCCESS){vk.destroyShaderModule(device,vertexShader,nullptr);vertexShader=VK_NULL_HANDLE;return false;}
    VkPushConstantRange push{};push.stageFlags=VK_SHADER_STAGE_VERTEX_BIT;push.offset=0;push.size=sizeof(Mat4)*2;VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pipelineLayoutInfo.setLayoutCount=1;pipelineLayoutInfo.pSetLayouts=&descriptorLayout;pipelineLayoutInfo.pushConstantRangeCount=1;pipelineLayoutInfo.pPushConstantRanges=&push;bool ok=vk.createPipelineLayout(device,&pipelineLayoutInfo,nullptr,&pipelineLayout)==VK_SUCCESS;
    if(ok)ok=createGraphicsPipeline(renderPass,VK_COMPARE_OP_LESS_OR_EQUAL,
        false,pipeline);
    return ok;
}

HandRenderer::Impl::ScenePipeline* HandRenderer::Impl::scenePipeline(
    const HandSceneTarget&target){
    for(auto&candidate:scenePipelines)
        if(candidate.colorFormat==target.colorFormat
            &&candidate.depthFormat==target.depthFormat
            &&candidate.reverseDepth==target.reverseDepth)return &candidate;
    if(target.samples!=VK_SAMPLE_COUNT_1_BIT)return nullptr;
    ScenePipeline candidate{};candidate.colorFormat=target.colorFormat;
    candidate.depthFormat=target.depthFormat;candidate.reverseDepth=target.reverseDepth;
    VkAttachmentDescription color{};color.format=target.colorFormat;
    color.samples=VK_SAMPLE_COUNT_1_BIT;color.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentDescription depth{};depth.format=target.depthFormat;
    depth.samples=target.samples;depth.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const std::array<VkAttachmentDescription,2>attachments{{color,depth}};
    VkAttachmentReference colorReference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthReference{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount=1;subpass.pColorAttachments=&colorReference;
    subpass.pDepthStencilAttachment=&depthReference;
    VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassInfo.attachmentCount=static_cast<std::uint32_t>(attachments.size());
    renderPassInfo.pAttachments=attachments.data();renderPassInfo.subpassCount=1;
    renderPassInfo.pSubpasses=&subpass;
    if(vk.createRenderPass(device,&renderPassInfo,nullptr,&candidate.renderPass)!=VK_SUCCESS)
        return nullptr;
    const VkCompareOp compare=target.reverseDepth?VK_COMPARE_OP_GREATER_OR_EQUAL
                                                 :VK_COMPARE_OP_LESS_OR_EQUAL;
    // DOOM's final WSI image is UNORM even though it contains display-encoded
    // pixels. The hand shader normally writes linear values to an sRGB OpenXR
    // target, so encode explicitly only when drawing into this native image.
    const bool encodeSrgb=target.colorFormat==VK_FORMAT_R8G8B8A8_UNORM
        ||target.colorFormat==VK_FORMAT_B8G8R8A8_UNORM
        ||target.colorFormat==VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    if(!createGraphicsPipeline(candidate.renderPass,compare,encodeSrgb,
            candidate.pipeline)){
        vk.destroyRenderPass(device,candidate.renderPass,nullptr);return nullptr;
    }
    scenePipelines.push_back(candidate);
    say(std::string("DOOM scene-depth integration ready colorFormat=")
        +std::to_string(target.colorFormat)+" depthFormat="
        +std::to_string(target.depthFormat)+(target.reverseDepth?" reverse-Z":" forward-Z")
        +(encodeSrgb?" manual-sRGB":" native-sRGB"));
    return &scenePipelines.back();
}

void HandRenderer::Impl::drawGeometry(VkCommandBuffer commandBuffer,
    VkPipeline selectedPipeline,VkExtent2D extent,const HandEyeView&view,
    const HandPose&leftGrip,const HandPose&rightGrip,
    const HandVisibilityOutput&visibility,const HandGameplayState&gameplay,const HandPose&laser){
    const bool drawLeft=visibility.left!=HandModelKind::None&&leftGrip.valid;
    const bool drawRight=visibility.right!=HandModelKind::None&&rightGrip.valid;
    if(!drawLeft&&!drawRight&&!laser.valid)return;
    const int32_t rectX=std::clamp(view.imageRectX,0,int32_t(extent.width));
    const int32_t rectY=std::clamp(view.imageRectY,0,int32_t(extent.height));
    const uint32_t requestedWidth=view.imageRectWidth?view.imageRectWidth:extent.width;
    const uint32_t requestedHeight=view.imageRectHeight?view.imageRectHeight:extent.height;
    const uint32_t rectWidth=std::min(requestedWidth,extent.width-uint32_t(rectX));
    const uint32_t rectHeight=std::min(requestedHeight,extent.height-uint32_t(rectY));
    if(!rectWidth||!rectHeight)return;
    VkRect2D scissor{{rectX,rectY},{rectWidth,rectHeight}};
    vk.cmdBindPipeline(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,selectedPipeline);
    VkViewport viewport{float(rectX),float(rectY),float(rectWidth),float(rectHeight),0,1};
    vk.cmdSetViewport(commandBuffer,0,1,&viewport);vk.cmdSetScissor(commandBuffer,0,1,&scissor);
    Mat4 viewProjection=multiply(projection(view),inversePose(view.pose));
    // DOOM uses reverse-Z on its main scene target. Map the hand projection's
    // [near=0, far=1] clip depth to [near=1, far=0] before the shared test.
    const bool reverse=selectedPipeline!=pipeline&&std::any_of(scenePipelines.begin(),scenePipelines.end(),[&](const ScenePipeline&candidate){return candidate.pipeline==selectedPipeline&&candidate.reverseDepth;});
    if(reverse)for(int column=0;column<4;column++)
        viewProjection.m[column*4+2]=viewProjection.m[column*4+3]-viewProjection.m[column*4+2];
    auto draw=[&](bool left,HandModelKind kind,const HandPose&pose){
        const bool grenadeHand=grenadeVisualEligible(grenadeVisualReady,gameplay.leftHanded,
            gameplay.interaction.grenadeHeld,gameplay.twoHanded,left,kind==HandModelKind::Fist);
        const size_t index=left?(kind==HandModelKind::GunHolding?2:0)
                               :(kind==HandModelKind::GunHolding?3:1);
        Asset* selectedAsset=&assets[index];
        const auto weaponIndex=static_cast<size_t>(gameplay.weapon);
        bool reviewed=weaponIndex<supportAssets.size()
            &&weaponSupportEligible(left,gameplay.leftHanded,kind==HandModelKind::GunHolding,gameplay.twoHanded)
            &&gameplay.interaction.supportModelValid&&supportAssets[weaponIndex][left?0:1].ready;
        auto reviewedModel=gameplay.interaction.supportModel;
        auto jointModel=[&](const WeaponSupportProfile& profile,unsigned hand,Mat4& model){
            model=gameplay.interaction.supportModel;
            const int slot=profile.jointSlot[hand];
            return slot<0||((gameplay.weaponJointMask&(1u<<slot))
                &&weaponJointModel(model,gameplay.weaponJoints[slot],profile.jointBindInverse[hand],model));
        };
        if(reviewed){
            const auto&profile=supportProfiles[weaponIndex];
            if(profile.metersPerAssetUnit>0){
                const auto hand=left?0:1;
                const float handScale=(left?leftCalibration:rightCalibration).scale;
                reviewed=jointModel(profile,hand,reviewedModel)&&fixedSizeSupportMatrix(reviewedModel,
                    left?profile.point:profile.rightPoint,
                    profile.metersPerAssetUnit*handScale/profile.referenceHandScales[hand],reviewedModel);
            }
        }
        bool primary=weaponIndex<primaryAssets.size()
            &&weaponPrimaryEligible(left,gameplay.leftHanded,kind==HandModelKind::GunHolding,
                gameplay.twoHanded,primaryProfiles[weaponIndex].valid)
            &&gameplay.interaction.supportModelValid&&primaryAssets[weaponIndex].ready;
        if(primary){
            const auto&profile=primaryProfiles[weaponIndex];
            primary=jointModel(profile,1,reviewedModel)&&fixedSizeSupportMatrix(reviewedModel,profile.rightPoint,
                profile.metersPerAssetUnit*rightCalibration.scale/profile.referenceHandScales[1],reviewedModel);
            if(primary){
                selectedAsset=&primaryAssets[weaponIndex];reviewed=true;
                const uint32_t bit=1u<<weaponIndex;
                if(!(reviewedPrimaryLogged&bit)){
                    reviewedPrimaryLogged|=bit;
                    say(std::string("manual primary active weapon=")+KharvoxWeaponKindKey(gameplay.weapon)
                        +" hand=right source=completed-native-weapon-model");
                }
            }
        }
        if(reviewed&&!primary)selectedAsset=&supportAssets[weaponIndex][left?0:1];
        if(reviewed&&!primary){
            const uint32_t bit=1u<<(weaponIndex*2+(left?0:1));
            if(!(reviewedSupportLogged&bit)){
                reviewedSupportLogged|=bit;
                say(std::string("reviewed support active weapon=")+KharvoxWeaponKindKey(gameplay.weapon)
                    +" hand="+(left?"left":"right")+" source=completed-native-weapon-model");
            }
        }
        if(grenadeHand){selectedAsset=&grenadeVisualHand;reviewed=false;}
        auto&asset=*selectedAsset;if(!asset.ready)return;
        const auto&calibration=renderCalibration(left,kind,gameplay.weapon,
            gameplay.leftHanded);
        const auto&fingers=fingerData[left?0:1];
        const bool attached=left!=gameplay.leftHanded&&kind==HandModelKind::GunHolding
            &&gameplay.interaction.support.valid&&fingers.loaded
            &&fingers.header.gunHash==asset.sourceHash;
        const auto model=grenadeHand?poseMatrix(pose):reviewed?reviewedModel:attached?attachedHandMatrix(gameplay.interaction.support,
            calibration,mirrorX[index],fingers.header.fistWrist,
            fingers.header.gunWrist,fingers.header.graspCentre)
            :calibratedHandMatrix(pose,calibration,mirrorX[index]);
        const auto push=handDrawPush(model,viewProjection,
            gameplay.interaction.fingers[left?0:1],!grenadeHand&&kind==HandModelKind::Fist&&fingers.loaded);
        vk.cmdPushConstants(commandBuffer,pipelineLayout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),&push);
        vk.cmdBindDescriptorSets(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&asset.descriptor,0,nullptr);
        VkDeviceSize offset{};vk.cmdBindVertexBuffers(commandBuffer,0,1,&asset.vertices,&offset);
        vk.cmdBindIndexBuffer(commandBuffer,asset.indices,0,VK_INDEX_TYPE_UINT32);
        vk.cmdDrawIndexed(commandBuffer,asset.indexCount,1,0,0,0);
    };
    if(drawLeft)draw(true,visibility.left,leftGrip);
    if(drawRight)draw(false,visibility.right,rightGrip);
    const bool leftSupport=!gameplay.leftHanded;
    const size_t support=leftSupport?0:1;
    if(grenadeVisualEligible(grenadeVisualReady,gameplay.leftHanded,
        gameplay.interaction.grenadeHeld,gameplay.twoHanded,true,
        drawLeft&&visibility.left==HandModelKind::Fist)){
        // Both assets are baked in physical Grip XR metres. Applying the
        // legacy hand calibration again would distort the validated pose.
        const auto push=handDrawPush(poseMatrix(leftGrip),viewProjection);
        vk.cmdPushConstants(commandBuffer,pipelineLayout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),&push);
        vk.cmdBindDescriptorSets(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&grenadeVisualAsset.descriptor,0,nullptr);
        VkDeviceSize offset{};vk.cmdBindVertexBuffers(commandBuffer,0,1,&grenadeVisualAsset.vertices,&offset);
        vk.cmdBindIndexBuffer(commandBuffer,grenadeVisualAsset.indices,0,VK_INDEX_TYPE_UINT32);
        vk.cmdDrawIndexed(commandBuffer,grenadeVisualAsset.indexCount,1,0,0,0);
    }
    if(!grenadeVisualReady&&gameplay.interaction.grenadeHeld&&grenadeAsset.ready&&fingerData[support].loaded
        &&(leftSupport?drawLeft:drawRight)
        &&(leftSupport?visibility.left:visibility.right)==HandModelKind::Fist){
        const auto&pose=leftSupport?leftGrip:rightGrip;
        const auto&calibration=renderCalibration(leftSupport,HandModelKind::Fist,
            gameplay.weapon,gameplay.leftHanded);
        const auto fist=calibratedHandMatrix(pose,calibration,mirrorX[support]);
        const auto&header=fingerData[support].header;
        const auto weights=handDrawPush(fist,viewProjection,gameplay.interaction.fingers[support],true);
        auto centre=header.fistGraspCentre;
        for(int shape=0;shape<3;++shape)for(int c=0;c<3;++c)
            centre[c]+=header.graspDeltas[shape][c]*weights.openness[shape];
        auto model=multiply(fist,header.fistWrist);
        for(int c=0;c<3;++c){
            float length{};for(int row=0;row<3;++row)length+=model.m[c*4+row]*model.m[c*4+row];
            length=std::sqrt(std::max(length,1e-8f));
            for(int row=0;row<3;++row)model.m[c*4+row]/=length;
        }
        for(int row=0;row<3;++row)model.m[12+row]=fist.m[12+row]
            +fist.m[row]*centre[0]+fist.m[4+row]*centre[1]+fist.m[8+row]*centre[2];
        const auto push=handDrawPush(model,viewProjection);
        vk.cmdPushConstants(commandBuffer,pipelineLayout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),&push);
        vk.cmdBindDescriptorSets(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&assets[support].descriptor,0,nullptr);
        VkDeviceSize offset{};vk.cmdBindVertexBuffers(commandBuffer,0,1,&grenadeAsset.vertices,&offset);
        vk.cmdBindIndexBuffer(commandBuffer,grenadeAsset.indices,0,VK_INDEX_TYPE_UINT32);
        vk.cmdDrawIndexed(commandBuffer,grenadeAsset.indexCount,1,0,0,0);
    }
    if(laser.valid&&laserAsset.ready){
        const auto descriptor=std::find_if(assets.begin(),assets.end(),[](const Asset& a){return a.ready;});
        if(descriptor!=assets.end()){
            const auto push=handDrawPush(poseMatrix(laser),viewProjection);
            vk.cmdPushConstants(commandBuffer,pipelineLayout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),&push);
            vk.cmdBindDescriptorSets(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&descriptor->descriptor,0,nullptr);
            VkDeviceSize offset{};vk.cmdBindVertexBuffers(commandBuffer,0,1,&laserAsset.vertices,&offset);
            vk.cmdBindIndexBuffer(commandBuffer,laserAsset.indices,0,VK_INDEX_TYPE_UINT32);
            vk.cmdDrawIndexed(commandBuffer,laserAsset.indexCount,1,0,0,0);
        }
    }
}

void HandRenderer::Impl::destroyAsset(Asset&asset){
    for(auto&texture:asset.textures){
        if(texture&&texture.use_count()==1){
            if(texture->view)vk.destroyImageView(device,texture->view,nullptr);
            if(texture->image)vk.destroyImage(device,texture->image,nullptr);
            if(texture->memory)vk.freeMemory(device,texture->memory,nullptr);
        }
        texture.reset();
    }
    if(asset.indices)vk.destroyBuffer(device,asset.indices,nullptr);if(asset.indexMemory)vk.freeMemory(device,asset.indexMemory,nullptr);if(asset.vertices)vk.destroyBuffer(device,asset.vertices,nullptr);if(asset.vertexMemory)vk.freeMemory(device,asset.vertexMemory,nullptr);asset={};
}

const HandCalibration&HandRenderer::Impl::renderCalibration(bool left,
    HandModelKind kind,KharvoxWeaponKind weapon,bool leftHanded)const{
    const auto selection=selectHandCalibrationProfile(left,leftHanded,weapon,
        kind==HandModelKind::GunHolding);
    if(selection.weaponSpecific)
        return weaponCalibrations[selection.weaponIndex][selection.handIndex];
    return left?leftCalibration:rightCalibration;
}

HandCalibration&HandRenderer::Impl::selectedCalibration(){
    const auto selection=selectHandCalibrationProfile(calibrateLeft,
        calibrationLeftHanded,calibrationWeapon,true);
    if(selection.weaponSpecific)
        return weaponCalibrations[selection.weaponIndex][selection.handIndex];
    return calibrateLeft?leftCalibration:rightCalibration;
}

const HandCalibration&HandRenderer::Impl::selectedCalibration()const{
    const auto selection=selectHandCalibrationProfile(calibrateLeft,
        calibrationLeftHanded,calibrationWeapon,true);
    if(selection.weaponSpecific)
        return weaponCalibrations[selection.weaponIndex][selection.handIndex];
    return calibrateLeft?leftCalibration:rightCalibration;
}

void HandRenderer::Impl::loadCalibrationFile(const wchar_t* filename, bool defaults){
    std::ifstream input(root/filename);
    // Repository defaults are the baseline. A legacy v1 save deliberately
    // uses its global wrists for every weapon; partial v2 saves override only
    // their named profiles, preserving all other accepted defaults.
    auto resetWeaponProfiles=[&]{
        for(auto&profile:weaponCalibrations){profile[0]=leftCalibration;
            profile[1]=rightCalibration;}
    };
    if(!input){if(defaults){resetWeaponProfiles();say("hand calibration defaults missing; using hand_models.cfg wrists");}return;}
    std::unordered_map<std::string,std::string>values;std::string line;
    while(std::getline(input,line)){line=trim(line);if(line.empty()||line[0]=='#')continue;const auto equals=line.find('=');if(equals!=std::string::npos)values[trim(line.substr(0,equals))]=trim(line.substr(equals+1));}
    auto vector3=[&](const std::string&key,float value[3]){const auto found=values.find(key);if(found==values.end())return false;std::istringstream stream(found->second);float parsed[3]{};if(!(stream>>parsed[0]>>parsed[1]>>parsed[2])||!std::isfinite(parsed[0])||!std::isfinite(parsed[1])||!std::isfinite(parsed[2]))return false;std::copy_n(parsed,3,value);return true;};
    const bool leftPosition=vector3("left_position",leftCalibration.position);
    const bool leftRotation=vector3("left_rotation",leftCalibration.rotationDegrees);
    const bool rightPosition=vector3("right_position",rightCalibration.position);
    const bool rightRotation=vector3("right_rotation",rightCalibration.rotationDegrees);
    if(defaults || values["version"] != "2") resetWeaponProfiles();
    unsigned profileValues{};
    for(int value=static_cast<int>(KharvoxWeaponKind::Pistol);
        value<static_cast<int>(KharvoxWeaponKind::Count);++value){
        const auto weapon=static_cast<KharvoxWeaponKind>(value);
        const std::string key=KharvoxWeaponKindKey(weapon);
        auto&profile=weaponCalibrations[static_cast<size_t>(weapon)];
        profileValues+=vector3(key+"_left_position",profile[0].position)?1u:0u;
        profileValues+=vector3(key+"_left_rotation",profile[0].rotationDegrees)?1u:0u;
        profileValues+=vector3(key+"_right_position",profile[1].position)?1u:0u;
        profileValues+=vector3(key+"_right_rotation",profile[1].rotationDegrees)?1u:0u;
    }
    if(leftPosition||leftRotation||rightPosition||rightRotation||profileValues)
        say(std::string(defaults?"default":"saved")+" hand calibration loaded; per-weapon values="
            +std::to_string(profileValues));
}

void HandRenderer::Impl::writeCalibrationStatus(const char*message)const{
    if(calibrationMode==CalibrationMode::None)return;
    const auto&value=selectedCalibration();
    const bool weaponProfile=selectHandCalibrationProfile(calibrateLeft,
        calibrationLeftHanded,calibrationWeapon,true).weaponSpecific;
    const auto path=root/L"hand_calibration_status.txt";
    std::ofstream output(path,std::ios::trunc);
    if(!output)return;
    output<<std::fixed<<std::setprecision(3)
        <<(calibrationMode==CalibrationMode::Rotation?"ROTATION":"POSITION")
        <<" | hand="<<(calibrateLeft?"LEFT":"RIGHT")
        <<" | profile="<<(weaponProfile?KharvoxWeaponKindKey(calibrationWeapon)
                                        :"global_fist")
        <<" | position="<<value.position[0]<<' '<<value.position[1]<<' '<<value.position[2]
        <<" | rotation="<<value.rotationDegrees[0]<<' '<<value.rotationDegrees[1]<<' '<<value.rotationDegrees[2];
    if(message&&*message)output<<" | "<<message;
}

void HandRenderer::Impl::publishSupportHudTemplates(){
    auto doomToXr=math::identity();
    doomToXr.m[0]=doomToXr.m[5]=doomToXr.m[10]=0;
    doomToXr.m[2]=-1;doomToXr.m[4]=-1;doomToXr.m[9]=1;
    HandPose origin{};origin.valid=true;
    auto xrToDoom=math::identity();
    for(int row=0;row<3;++row)for(int column=0;column<3;++column)xrToDoom.m[column*4+row]=doomToXr.m[row*4+column];
    const auto envelope=[](const Asset& asset,const math::Mat4& matrix){
        kharvox::OffhandHudBounds bounds;bounds.sectionMask=asset.hudSectionMask;
        for(int part=0;part<=8;++part){
            if(part&&!(asset.hudSectionMask&(1u<<(part-1))))continue;
            for(int corner=0;corner<8;++corner){
                float point[3]{};for(int i=0;i<3;++i)point[i]=part
                    ?asset.hudSections[part-1][i+((corner&(1<<i))?3:0)]
                    :((corner&(1<<i))?asset.maximum[i]:asset.minimum[i]);
                transformPosition(matrix.m,point);
                for(int i=0;i<3;++i){point[i]*=100.f;
                    if(part){auto& section=bounds.sections[part-1];
                        section[i]=corner?std::min(section[i],point[i]):point[i];
                        section[3+i]=corner?std::max(section[3+i],point[i]):point[i];
                    }else{
                        bounds.minimum[i]=corner?std::min(bounds.minimum[i],point[i]):point[i];
                        bounds.maximum[i]=corner?std::max(bounds.maximum[i],point[i]):point[i];
                    }
                }
            }
        }
        return bounds;
    };
    for(size_t hand=0;hand<2;++hand){
        const auto&asset=assets[hand];
        const auto matrix=multiply(xrToDoom,calibratedHandMatrix(origin,
            hand?rightCalibration:leftCalibration,mirrorX[hand]));
        const auto bounds=envelope(asset,matrix);
        KharvoxHudSetFreeHandBounds(hand==0,bounds,asset.ready);
    }
    for(size_t weapon=1;weapon<supportProfiles.size();++weapon)for(size_t hand=0;hand<2;++hand){
        SupportHudGeometry geometry{};const auto&profile=supportProfiles[weapon];
        const auto&asset=supportAssets[weapon][hand];
        const auto&calibration=hand?rightCalibration:leftCalibration;
        math::Mat4 inverseCalibration{},assetToGrip{};
        if(profile.hasHudFrame&&asset.ready&&inverseSupportSimilarity(
            calibratedHandMatrix(origin,calibration,mirrorX[hand]),inverseCalibration)){
            geometry.gripToAsset=multiply(profile.fistToAsset[hand],multiply(inverseCalibration,doomToXr));
            geometry.anchor=hand?profile.rightPoint:profile.point;
            geometry.jointSlot=profile.jointSlot[hand];
            geometry.jointBindInverse=profile.jointBindInverse[hand];
            geometry.metersPerAssetUnit=profile.metersPerAssetUnit*calibration.scale/profile.referenceHandScales[hand];
            if(inverseSupportSimilarity(geometry.gripToAsset,assetToGrip)){
                const auto bounds=envelope(asset,assetToGrip);
                geometry.minimumCm=bounds.minimum;geometry.maximumCm=bounds.maximum;
                geometry.hudSections=bounds.sections;geometry.hudSectionMask=bounds.sectionMask;
                geometry.valid=true;
            }
        }
        KharvoxHudSetSupportGeometry(static_cast<unsigned>(weapon),hand==0,geometry);
    }
}

void HandRenderer::Impl::saveCalibration(){
    publishSupportHudTemplates();
    const auto target=root/L"hand_models_calibration_saved.cfg";
    const auto temporary=root/L"hand_models_calibration_saved.tmp";
    {
        std::ofstream output(temporary,std::ios::trunc);
        if(!output){say("hand calibration save failed");return;}
        output<<"# Auto-saved KHARVOX hand calibration.\n"
            <<"# Gun poses are stored per weapon and physical hand; fist poses remain global.\n"
            <<"version=2\n"<<std::fixed<<std::setprecision(3)
            <<"left_position="<<leftCalibration.position[0]<<' '<<leftCalibration.position[1]<<' '<<leftCalibration.position[2]<<'\n'
            <<"left_rotation="<<leftCalibration.rotationDegrees[0]<<' '<<leftCalibration.rotationDegrees[1]<<' '<<leftCalibration.rotationDegrees[2]<<'\n'
            <<"right_position="<<rightCalibration.position[0]<<' '<<rightCalibration.position[1]<<' '<<rightCalibration.position[2]<<'\n'
            <<"right_rotation="<<rightCalibration.rotationDegrees[0]<<' '<<rightCalibration.rotationDegrees[1]<<' '<<rightCalibration.rotationDegrees[2]<<'\n';
        for(int value=static_cast<int>(KharvoxWeaponKind::Pistol);
            value<static_cast<int>(KharvoxWeaponKind::Count);++value){
            const auto weapon=static_cast<KharvoxWeaponKind>(value);
            const auto&profile=weaponCalibrations[static_cast<size_t>(weapon)];
            const std::string key=KharvoxWeaponKindKey(weapon);
            output<<key<<"_left_position="<<profile[0].position[0]<<' '
                <<profile[0].position[1]<<' '<<profile[0].position[2]<<'\n'
                <<key<<"_left_rotation="<<profile[0].rotationDegrees[0]<<' '
                <<profile[0].rotationDegrees[1]<<' '<<profile[0].rotationDegrees[2]<<'\n'
                <<key<<"_right_position="<<profile[1].position[0]<<' '
                <<profile[1].position[1]<<' '<<profile[1].position[2]<<'\n'
                <<key<<"_right_rotation="<<profile[1].rotationDegrees[0]<<' '
                <<profile[1].rotationDegrees[1]<<' '<<profile[1].rotationDegrees[2]<<'\n';
        }
    }
    if(!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        DeleteFileW(temporary.c_str());say("hand calibration save failed");return;
    }
    writeCalibrationStatus("saved automatically");
}

void HandRenderer::Impl::pollCalibration(const HandGameplayState&gameplay){
    if(calibrationMode==CalibrationMode::None)return;
    const bool contextChanged=calibrationWeapon!=gameplay.weapon
        ||calibrationLeftHanded!=gameplay.leftHanded;
    calibrationWeapon=gameplay.weapon;
    calibrationLeftHanded=gameplay.leftHanded;
    if(!calibrationContextInitialized){
        // Start on the visible weapon hand. Num 0 remains available for the
        // global free-fist profile, but no initial hidden-hand adjustment is
        // required in either handedness mode.
        calibrateLeft=gameplay.leftHanded;
        calibrationContextInitialized=true;
    }
    if(contextChanged)writeCalibrationStatus("active weapon/hand profile selected");
    DWORD foregroundProcess{};
    GetWindowThreadProcessId(GetForegroundWindow(),&foregroundProcess);
    const bool focused=foregroundProcess==GetCurrentProcessId();
    // Modified numpad belongs to HUD calibration, not hand/weapon calibration.
    const bool hudModifier=(GetAsyncKeyState(VK_MENU)&0x8000)||(GetAsyncKeyState(VK_CONTROL)&0x8000);
    const bool plusDown=(GetAsyncKeyState(VK_ADD)&0x8000)!=0;
    const bool handDown=(GetAsyncKeyState(VK_NUMPAD0)&0x8000)!=0;
    const bool resetDown=(GetAsyncKeyState(VK_NUMPAD5)&0x8000)!=0;
    if(!focused||hudModifier||KharvoxHudOffhandCalibrationActive()){
        calibrationPlusWasDown=plusDown;calibrationHandWasDown=handDown;
        calibrationResetWasDown=resetDown;return;
    }
    const auto nextMode=updateHandCalibrationMode(calibrationMode,plusDown,
        calibrationPlusWasDown);
    if(nextMode!=calibrationMode){
        calibrationMode=nextMode;
        writeCalibrationStatus("Num + switched rotation / position");
    }
    const bool switchHand=handDown&&!calibrationHandWasDown;
    const bool reset=resetDown&&!calibrationResetWasDown;
    calibrationHandWasDown=handDown;calibrationResetWasDown=resetDown;
    if(switchHand){calibrateLeft=!calibrateLeft;writeCalibrationStatus("Num 0 switched hand");}
    if(reset){
        auto&value=selectedCalibration();
        const auto selection=selectHandCalibrationProfile(calibrateLeft,
            calibrationLeftHanded,calibrationWeapon,true);
        value=selection.weaponSpecific
            ?defaultWeaponCalibrations[selection.weaponIndex][selection.handIndex]
            :(calibrateLeft?defaultLeftCalibration:defaultRightCalibration);
        saveCalibration();return;
    }
    const ULONGLONG now=GetTickCount64();
    if(now-lastCalibrationStep<90)return;
    const bool fine=(GetAsyncKeyState(VK_SHIFT)&0x8000)!=0;
    const std::array<int,6>keys{{VK_NUMPAD4,VK_NUMPAD6,VK_NUMPAD2,VK_NUMPAD8,VK_NUMPAD7,VK_NUMPAD9}};
    std::array<bool,6>down{};bool any{};for(size_t i=0;i<keys.size();i++){down[i]=(GetAsyncKeyState(keys[i])&0x8000)!=0;any|=down[i];}
    if(!any)return;
    auto&value=selectedCalibration();
    float*components=calibrationMode==CalibrationMode::Rotation?value.rotationDegrees:value.position;
    const float step=calibrationMode==CalibrationMode::Rotation?(fine?1.f:5.f):(fine?.001f:.005f);
    if(down[0])components[0]-=step;if(down[1])components[0]+=step;
    if(down[2])components[1]-=step;if(down[3])components[1]+=step;
    // OpenXR forward is -Z: Num 7 moves the hand farther forward, Num 9 back.
    if(down[4])components[2]-=step;if(down[5])components[2]+=step;
    if(calibrationMode==CalibrationMode::Rotation)for(float&component:value.rotationDegrees)component=std::remainder(component,360.f);
    else for(float&component:value.position)component=std::clamp(component,-.5f,.5f);
    lastCalibrationStep=now;saveCalibration();
}

HandRenderer::HandRenderer():impl_(new Impl){}
HandRenderer::~HandRenderer(){shutdown();delete impl_;}

bool HandRenderer::initialize(VkPhysicalDevice physicalDevice,VkDevice device,
    VkQueue queue,std::uint32_t queueFamily,const KharvoxVulkanDispatch&dispatch,
    VkFormat colorFormat,const std::array<VkExtent2D,2>&eyeExtents,
    const std::array<std::vector<VkImage>,2>&eyeImages,
    const std::wstring&runtimeDirectory,HandLog logger){
    shutdown();impl_->physical=physicalDevice;impl_->device=device;impl_->queue=queue;impl_->queueFamily=queueFamily;impl_->vk=dispatch;impl_->format=colorFormat;impl_->extents=eyeExtents;impl_->root=runtimeDirectory;impl_->log=std::move(logger);
    impl_->fingerData={};
    std::unordered_map<std::string,std::string>config;std::ifstream input(impl_->root/L"hand_models.cfg");std::string line;while(std::getline(input,line)){line=trim(line);if(line.empty()||line[0]=='#')continue;const auto equals=line.find('=');if(equals!=std::string::npos)config[trim(line.substr(0,equals))]=trim(line.substr(equals+1));}
    if(!input&&!std::filesystem::exists(impl_->root/L"hand_models.cfg")){impl_->say("hand_models.cfg missing; static hands disabled");return false;}
    auto vector3=[&](const char*key,float value[3]){std::istringstream stream(config[key]);return bool(stream>>value[0]>>value[1]>>value[2]);};vector3("left_position",impl_->leftCalibration.position);vector3("left_rotation",impl_->leftCalibration.rotationDegrees);vector3("right_position",impl_->rightCalibration.position);vector3("right_rotation",impl_->rightCalibration.rotationDegrees);try{if(config.count("left_scale"))impl_->leftCalibration.scale=std::clamp(std::stof(config["left_scale"]),.05f,2.f);if(config.count("right_scale"))impl_->rightCalibration.scale=std::clamp(std::stof(config["right_scale"]),.05f,2.f);}catch(...){impl_->say("invalid hand scale; using safe defaults");}impl_->mirrorX[0]=config["left_fist_mirror_x"]=="1";impl_->mirrorX[1]=config["right_fist_mirror_x"]=="1";impl_->mirrorX[2]=config["left_gun_mirror_x"]=="1";impl_->mirrorX[3]=config["right_gun_mirror_x"]=="1";
    impl_->loadCalibrationFile(L"hand_models_calibration_default.cfg",true);
    impl_->defaultLeftCalibration=impl_->leftCalibration;impl_->defaultRightCalibration=impl_->rightCalibration;
    impl_->defaultWeaponCalibrations=impl_->weaponCalibrations;
    impl_->loadCalibrationFile(L"hand_models_calibration_saved.cfg",false);
    char calibrationMode[24]{};
    if(GetEnvironmentVariableA("KHARVOX_HAND_CALIBRATION",calibrationMode,sizeof(calibrationMode))>0){
        if(!_stricmp(calibrationMode,"rotation"))impl_->calibrationMode=CalibrationMode::Rotation;
        else if(!_stricmp(calibrationMode,"position"))impl_->calibrationMode=CalibrationMode::Position;
    }
    if(!impl_->createCommonResources()){impl_->say("Vulkan setup failed; native game rendering continues");shutdown();return false;}
    const std::array<const char*,4>keys{{"left_fist","right_fist","left_gun","right_gun"}};for(size_t i=0;i<keys.size();i++){const auto found=config.find(keys[i]);if(found==config.end()||found->second.empty()){impl_->say(std::string(keys[i])+" not configured; that hand pose is disabled");continue;}const auto path=impl_->root/std::filesystem::u8path(found->second);if(!std::filesystem::exists(path)){impl_->say(path.filename().u8string()+" missing; model disabled");continue;}impl_->loadAsset(path,impl_->assets[i]);}
    std::array<WeaponSupportProfile,15> supportProfiles{};
    std::ifstream supportConfig(impl_->root/L"support_hands.cfg");
    const auto supportCount=readWeaponSupportProfiles(supportConfig,supportProfiles);
    for(size_t weapon=1;weapon<supportProfiles.size();++weapon){
        const auto&profile=supportProfiles[weapon];if(!profile.valid)continue;
        for(size_t hand=0;hand<2;++hand){
            if(!profile.hashes[hand])continue; // explicitly retain a previously calibrated hand
            if(!impl_->assets[hand].ready)continue;
            if(profile.metersPerAssetUnit>0&&profile.fistHashes[hand]!=impl_->assets[hand].sourceHash){
                impl_->say("support physical-scale reference mismatch; retaining calibrated hand fallback");continue;
            }
            auto&asset=impl_->supportAssets[weapon][hand];
            if(!impl_->loadAsset(impl_->root/std::filesystem::u8path(profile.assetPaths[hand]),asset,&impl_->assets[hand])
                ||asset.sourceHash!=profile.hashes[hand]){
                impl_->say("support profile missing or hash mismatch; retaining calibrated hand fallback");
                impl_->destroyAsset(asset);
            }
        }
        impl_->supportProfiles[weapon]=profile;
    }
    // Reuse the existing fixed-size profile schema and physical-right atlas.
    // These optional meshes are uploaded once; no per-frame posing or I/O.
    std::array<WeaponSupportProfile,15> primaryProfiles{};
    std::ifstream primaryConfig(impl_->root/L"primary_hands.cfg");
    const auto primaryCount=readWeaponSupportProfiles(primaryConfig,primaryProfiles);
    for(size_t weapon=1;weapon<primaryProfiles.size();++weapon){
        const auto&profile=primaryProfiles[weapon];
        if(!profile.valid||!profile.hashes[1]||profile.metersPerAssetUnit<=0
            ||!impl_->assets[1].ready||profile.fistHashes[1]!=impl_->assets[1].sourceHash)continue;
        auto&asset=impl_->primaryAssets[weapon];
        if(!impl_->loadAsset(impl_->root/std::filesystem::u8path(profile.assetPaths[1]),asset,&impl_->assets[1])
            ||asset.sourceHash!=profile.hashes[1]){
            impl_->say("primary profile missing or hash mismatch; retaining calibrated hand fallback");
            impl_->destroyAsset(asset);continue;
        }
        impl_->primaryProfiles[weapon]=profile;
    }
    if(primaryCount)impl_->say("manual weapon-local primary profiles="+std::to_string(primaryCount)
        +"; right hand only, existing atlas shared");
    impl_->publishSupportHudTemplates();
    if(supportCount)impl_->say("reviewed weapon-local support profiles="+std::to_string(supportCount)+"; existing atlas shared, no runtime skinning");
    // One closed beam mesh: no crossed compositor ribbons or orientation singularity.
    std::vector<Vertex> beamVertices;
    std::vector<uint32_t> beamIndices;
    for(unsigned ring=0;ring<2;++ring)for(unsigned i=0;i<8;++i){
        const float angle=float(i)*6.28318530718f/8.f;
        Vertex v{};v.position[0]=.002f*std::cos(angle);v.position[1]=.002f*std::sin(angle);
        v.position[2]=ring?-20.f:0.f;v.normal[0]=std::cos(angle);v.normal[1]=std::sin(angle);
        v.baseColor[0]=1.f;v.baseColor[1]=.005f;v.baseColor[2]=.002f;v.material[0]=-1.f;
        beamVertices.push_back(v);
    }
    for(uint32_t i=0;i<8;++i){const auto n=(i+1)%8;
        for(auto v:{i,n,i+8,n,n+8,i+8})beamIndices.push_back(v);
    }
    impl_->laserAsset.ready=impl_->buffer(beamVertices.size()*sizeof(Vertex),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        beamVertices.data(),impl_->laserAsset.vertices,impl_->laserAsset.vertexMemory)
        &&impl_->buffer(beamIndices.size()*sizeof(uint32_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        beamIndices.data(),impl_->laserAsset.indices,impl_->laserAsset.indexMemory);
    impl_->laserAsset.indexCount=static_cast<uint32_t>(beamIndices.size());
    if(!impl_->createGrenadeAsset())impl_->say("held grenade preview unavailable; native throw remains active");
    GrenadeVisualConfig grenadeConfig;
    std::ifstream grenadeInput(impl_->root/L"grenade_visual_test.cfg");
    if(readGrenadeVisualConfig(grenadeInput,grenadeConfig)&&grenadeConfig.enabled){
        const auto handPath=impl_->root/L"assets/3D/PRAETOR_GRENADE_LEFT.glb";
        const auto grenadePath=impl_->root/L"assets/3D/GRENADE_FRAG.glb";
        std::vector<std::uint8_t> handBytes,grenadeBytes;
        const bool hashes=readBytes(handPath,handBytes)&&readBytes(grenadePath,grenadeBytes)
            &&handAssetHash(handBytes.data(),handBytes.size())==grenadeConfig.handHash
            &&handAssetHash(grenadeBytes.data(),grenadeBytes.size())==grenadeConfig.grenadeHash;
        impl_->grenadeVisualReady=hashes&&impl_->loadAsset(handPath,impl_->grenadeVisualHand,&impl_->assets[0])
            &&impl_->loadAsset(grenadePath,impl_->grenadeVisualAsset);
        if(!impl_->grenadeVisualReady){impl_->destroyAsset(impl_->grenadeVisualHand);impl_->destroyAsset(impl_->grenadeVisualAsset);}
        impl_->say(impl_->grenadeVisualReady
            ?"authored frag visual test ready; left hand only, no native throw changes"
            :"authored frag visual test disabled: assets missing, changed or invalid");
    }
    for (size_t eye = 0; eye < 2; eye++) {
      impl_->eyeViews[eye].resize(eyeImages[eye].size());
      impl_->depthTargets[eye].resize(eyeImages[eye].size());
      impl_->framebuffers[eye].resize(eyeImages[eye].size());
      for (size_t index = 0; index < eyeImages[eye].size(); index++) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = eyeImages[eye][index];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = colorFormat;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (impl_->vk.createImageView(device, &view, nullptr,
                                      &impl_->eyeViews[eye][index]) !=
            VK_SUCCESS)
          continue;
        if(!impl_->depthTarget(eyeExtents[eye],impl_->depthTargets[eye][index])){
          impl_->say("depth buffer creation failed; hand surface disabled for one eye image");
          continue;
        }
        const std::array<VkImageView,2>attachments{{impl_->eyeViews[eye][index],impl_->depthTargets[eye][index].view}};
        VkFramebufferCreateInfo framebuffer{
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = impl_->renderPass;
        framebuffer.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebuffer.pAttachments = attachments.data();
        framebuffer.width = eyeExtents[eye].width;
        framebuffer.height = eyeExtents[eye].height;
        framebuffer.layers = 1;
        impl_->vk.createFramebuffer(device, &framebuffer, nullptr,
                                    &impl_->framebuffers[eye][index]);
      }
    }
    const auto available=availability();impl_->initialized=available.leftFist||available.rightFist||available.leftGun||available.rightGun;impl_->say(std::string("renderer ")+(impl_->initialized?"ready":"has no usable assets")+"; depth-tested opaque hands, constant environment lighting, no idTech 6 lights or shadows");impl_->writeCalibrationStatus("Num 0 switches hand; Num 5 resets selected hand");return impl_->initialized;
}

void HandRenderer::shutdown() {
  if (!impl_ || !impl_->device)
    return;
  if (impl_->queue && impl_->vk.queueWaitIdle)
    impl_->vk.queueWaitIdle(impl_->queue);
  for(auto framebuffer:impl_->sceneFramebuffers)
    if(framebuffer)impl_->vk.destroyFramebuffer(impl_->device,framebuffer,nullptr);
  for(auto& copy:impl_->sceneDepthCopies)impl_->destroyDepth(copy.target);
  for(auto&scene:impl_->scenePipelines){
    if(scene.pipeline)impl_->vk.destroyPipeline(impl_->device,scene.pipeline,nullptr);
    if(scene.renderPass)impl_->vk.destroyRenderPass(impl_->device,scene.renderPass,nullptr);
  }
  for (auto &eye : impl_->framebuffers)
    for (auto framebuffer : eye)
      if (framebuffer)
        impl_->vk.destroyFramebuffer(impl_->device, framebuffer, nullptr);
  for (auto &eye : impl_->eyeViews)
    for (auto view : eye)
      if (view)
        impl_->vk.destroyImageView(impl_->device, view, nullptr);
  for(auto&eye:impl_->depthTargets)for(auto&depth:eye){
    if(depth.view)impl_->vk.destroyImageView(impl_->device,depth.view,nullptr);
    if(depth.image)impl_->vk.destroyImage(impl_->device,depth.image,nullptr);
    if(depth.memory)impl_->vk.freeMemory(impl_->device,depth.memory,nullptr);
  }
  impl_->destroyAsset(impl_->laserAsset);
  impl_->destroyAsset(impl_->grenadeAsset);
  impl_->destroyAsset(impl_->grenadeVisualHand);
  impl_->destroyAsset(impl_->grenadeVisualAsset);
  impl_->grenadeVisualReady=false;
  for (auto &asset : impl_->assets)
    impl_->destroyAsset(asset);
  for(auto& weapon:impl_->supportAssets)for(auto& asset:weapon)
    impl_->destroyAsset(asset);
  for(auto& asset:impl_->primaryAssets)impl_->destroyAsset(asset);
  impl_->publishSupportHudTemplates();
  impl_->textureCache.clear();
  if (impl_->pipeline)
    impl_->vk.destroyPipeline(impl_->device, impl_->pipeline, nullptr);
  if(impl_->fragmentShader)
    impl_->vk.destroyShaderModule(impl_->device,impl_->fragmentShader,nullptr);
  if(impl_->vertexShader)
    impl_->vk.destroyShaderModule(impl_->device,impl_->vertexShader,nullptr);
  if (impl_->pipelineLayout)
    impl_->vk.destroyPipelineLayout(impl_->device, impl_->pipelineLayout,
                                    nullptr);
  if (impl_->renderPass)
    impl_->vk.destroyRenderPass(impl_->device, impl_->renderPass, nullptr);
  if (impl_->descriptorPool)
    impl_->vk.destroyDescriptorPool(impl_->device, impl_->descriptorPool,
                                    nullptr);
  if (impl_->descriptorLayout)
    impl_->vk.destroyDescriptorSetLayout(impl_->device, impl_->descriptorLayout,
                                         nullptr);
  if (impl_->sampler)
    impl_->vk.destroySampler(impl_->device, impl_->sampler, nullptr);
  if (impl_->uploadPool)
    impl_->vk.destroyCommandPool(impl_->device, impl_->uploadPool, nullptr);
  const auto logger = impl_->log;
  *impl_ = Impl{};
  impl_->log = logger;
  if (logger)
    logger("[HANDS] renderer resources released");
}

HandAssetAvailability HandRenderer::availability()const{return {impl_->assets[0].ready,impl_->assets[1].ready,impl_->assets[2].ready,impl_->assets[3].ready};}
bool HandRenderer::hasGrenadeVisualPose()const{return impl_->initialized&&impl_->grenadeVisualReady;}
bool HandRenderer::hasSupportProfile(KharvoxWeaponKind weapon,bool leftHanded)const{
    const auto index=static_cast<size_t>(weapon);
    return index<impl_->supportAssets.size()&&impl_->supportAssets[index][leftHanded?1:0].ready;
}
bool HandRenderer::hasPrimaryProfile(KharvoxWeaponKind weapon)const{
    const auto index=static_cast<size_t>(weapon);
    return index<impl_->primaryAssets.size()&&impl_->primaryProfiles[index].valid
        &&impl_->primaryAssets[index].ready;
}
const HandCalibration&HandRenderer::leftCalibration()const{return impl_->leftCalibration;}
const HandCalibration&HandRenderer::rightCalibration()const{return impl_->rightCalibration;}

bool HandRenderer::authoredSupportGrip(KharvoxWeaponKind weapon,bool leftHanded,
    const math::Mat4& jointModel,std::array<float,3>& point)const{
    const auto index=static_cast<size_t>(weapon);const unsigned hand=leftHanded?1:0;
    if(index>=impl_->supportProfiles.size()||!hasSupportProfile(weapon,leftHanded))return false;
    const auto& profile=impl_->supportProfiles[index];
    const auto& calibration=hand?impl_->rightCalibration:impl_->leftCalibration;
    if(!profile.valid||!profile.hasHudFrame||profile.referenceHandScales[hand]<=0)return false;
    return authoredSupportGripPoint(jointModel,hand?profile.rightPoint:profile.point,
        profile.metersPerAssetUnit*calibration.scale/profile.referenceHandScales[hand],
        profile.fistToAsset[hand],calibration,impl_->mirrorX[hand],point);
}

void HandRenderer::record(VkCommandBuffer commandBuffer,std::uint32_t eye,
    std::uint32_t imageIndex,const HandEyeView&view,const HandPose&leftGrip,
    const HandPose&rightGrip,const HandVisibilityOutput&visibility,
    const HandGameplayState&gameplay){
    if(!impl_->initialized||eye>=2||imageIndex>=impl_->framebuffers[eye].size()||!impl_->framebuffers[eye][imageIndex])return;impl_->pollCalibration(gameplay);const bool drawLeft=visibility.left!=HandModelKind::None&&leftGrip.valid;const bool drawRight=visibility.right!=HandModelKind::None&&rightGrip.valid;if(!drawLeft&&!drawRight)return;
    const auto&extent=impl_->extents[eye];
    const int32_t rectX=std::clamp(view.imageRectX,0,int32_t(extent.width));
    const int32_t rectY=std::clamp(view.imageRectY,0,int32_t(extent.height));
    const uint32_t requestedWidth=view.imageRectWidth?view.imageRectWidth:extent.width;
    const uint32_t requestedHeight=view.imageRectHeight?view.imageRectHeight:extent.height;
    const uint32_t rectWidth=std::min(requestedWidth,extent.width-uint32_t(rectX));
    const uint32_t rectHeight=std::min(requestedHeight,extent.height-uint32_t(rectY));
    if(!rectWidth||!rectHeight)return;
    VkRect2D scissor{{rectX,rectY},{rectWidth,rectHeight}};
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = impl_->renderPass;
    begin.framebuffer = impl_->framebuffers[eye][imageIndex];
    begin.renderArea = scissor;
    std::array<VkClearValue,2>clearValues{};clearValues[1].depthStencil={1.f,0};
    begin.clearValueCount=static_cast<uint32_t>(clearValues.size());
    begin.pClearValues=clearValues.data();
    impl_->vk.cmdBeginRenderPass(commandBuffer, &begin,
                                 VK_SUBPASS_CONTENTS_INLINE);
    impl_->drawGeometry(commandBuffer,impl_->pipeline,extent,view,leftGrip,
        rightGrip,visibility,gameplay);
    impl_->vk.cmdEndRenderPass(commandBuffer);
}

bool HandRenderer::recordSceneIntegrated(VkCommandBuffer commandBuffer,
    const HandSceneTarget&target,const HandEyeView&view,
    const HandPose&leftGrip,const HandPose&rightGrip,
    const HandVisibilityOutput&visibility,const HandGameplayState&gameplay,const HandPose&laser){
    if(!impl_->initialized||!target.colorView||!target.depthView
        ||target.colorFormat==VK_FORMAT_UNDEFINED
        ||!handSceneDepthFormat(target.depthFormat)
        ||target.samples!=VK_SAMPLE_COUNT_1_BIT)return false;
    if(!handSceneHasGeometry(visibility,leftGrip,rightGrip,laser))return false;
    impl_->pollCalibration(gameplay);
    auto*scene=impl_->scenePipeline(target);if(!scene)return false;
    Impl::SceneDepth* privateDepth=nullptr;
    if(target.copyDepthForHands){
        const auto slot=impl_->sceneDepthCopiesUsed++;
        if(slot==impl_->sceneDepthCopies.size())impl_->sceneDepthCopies.emplace_back();
        privateDepth=&impl_->sceneDepthCopies[slot];
        if(privateDepth->format!=target.depthFormat||privateDepth->extent.width!=target.extent.width
            ||privateDepth->extent.height!=target.extent.height){
            // This slot is reused only after finishSceneIntegratedFrame, which
            // follows the owner fence and destroys all borrowing framebuffers.
            impl_->destroyDepth(privateDepth->target);*privateDepth={};
        }
        if(!privateDepth->target.view){
            if(!impl_->depthTarget(target.extent,privateDepth->target,target.depthFormat,true)){
                impl_->destroyDepth(privateDepth->target);return false;
            }
            privateDepth->format=target.depthFormat;privateDepth->extent=target.extent;
            impl_->say("Native private hand depth ready; scene depth/stencil preserved");
        }
    }
    const std::array<VkImageView,2>attachments{{target.colorView,privateDepth?privateDepth->target.view:target.depthView}};
    VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebufferInfo.renderPass=scene->renderPass;
    framebufferInfo.attachmentCount=static_cast<std::uint32_t>(attachments.size());
    framebufferInfo.pAttachments=attachments.data();framebufferInfo.width=target.extent.width;
    framebufferInfo.height=target.extent.height;framebufferInfo.layers=1;
    VkFramebuffer framebuffer{};
    if(impl_->vk.createFramebuffer(impl_->device,&framebufferInfo,nullptr,&framebuffer)!=VK_SUCCESS)
        return false;
    impl_->sceneFramebuffers.push_back(framebuffer);
    if(privateDepth){
        copyHandSceneDepth(impl_->vk,commandBuffer,target.depthImage,privateDepth->target.image,
            target.extent,handSceneDepthAspect(target.depthFormat),privateDepth->initialized,target.depthArrayLayer);
        privateDepth->initialized=true;
    }
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass=scene->renderPass;begin.framebuffer=framebuffer;
    begin.renderArea={{0,0},target.extent};
    impl_->vk.cmdBeginRenderPass(commandBuffer,&begin,VK_SUBPASS_CONTENTS_INLINE);
    impl_->drawGeometry(commandBuffer,scene->pipeline,target.extent,view,leftGrip,
        rightGrip,visibility,gameplay,laser);
    impl_->vk.cmdEndRenderPass(commandBuffer);
    return true;
}

void HandRenderer::finishSceneIntegratedFrame(){
    if(!impl_->device)return;
    for(auto framebuffer:impl_->sceneFramebuffers)
        if(framebuffer)impl_->vk.destroyFramebuffer(impl_->device,framebuffer,nullptr);
    impl_->sceneFramebuffers.clear();
    impl_->sceneDepthCopiesUsed=0;
}

} // namespace kharvox::hands
