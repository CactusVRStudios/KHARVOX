#include "../src/vulkan/VirtualTextureDiagnostics.h"
#include "../src/vulkan/TextureQualityDiagnostics.h"
#include <cstdlib>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#include <new>
#endif

namespace {
void check(bool value){if(!value)std::abort();}

void batchedTextureUploads() {
    using namespace kharvox;
    TextureQualityDiagnostics textures;
    VkBufferImageCopy copies[4]{};
    copies[0].imageSubresource.mipLevel=4;
    copies[1].imageSubresource.mipLevel=4;
    copies[2].imageSubresource.mipLevel=2;
    copies[3].imageSubresource.mipLevel=2;
    textures.upload(7,4,copies);
    check(textures.statistics().uploadRegions==4
        &&textures.statistics().uploadMipTransitions==1
        &&textures.statistics().uploadedMips[4]==2&&textures.statistics().uploadedMips[2]==2);
    copies[0].imageSubresource.mipLevel=2;
    copies[1].imageSubresource.mipLevel=3;
    copies[2].imageSubresource.mipLevel=3;
    copies[3].imageSubresource.mipLevel=4;
    textures.upload(7,4,copies); // continues from the preceding batch's last mip
    check(textures.statistics().uploadRegions==8
        &&textures.statistics().uploadMipTransitions==3
        &&textures.statistics().uploadedMips[2]==3&&textures.statistics().uploadedMips[3]==2
        &&textures.statistics().uploadedMips[4]==3);
    textures.forgetImage(7);textures.upload(7,4,copies);
    check(textures.statistics().uploadRegions==12
        &&textures.statistics().uploadMipTransitions==5); // no stale 4 -> 2 transition

    TextureQualityDiagnostics bounded;
    bounded.upload(5000,0,copies);
    bounded.upload(5001,4,nullptr);
    check(bounded.statistics().uploadRegions==0);
    VkBufferImageCopy single{};single.imageSubresource.mipLevel=1;
    for(unsigned image=0;image<1024;++image)bounded.upload(image,1,&single);
    check(bounded.statistics().untrackedImages==0); // empty uploads consumed no slots
    copies[0].imageSubresource.mipLevel=1;
    copies[1].imageSubresource.mipLevel=2;
    copies[2].imageSubresource.mipLevel=2;
    bounded.upload(2048,3,copies);
    check(bounded.statistics().untrackedImages==3
        &&bounded.statistics().uploadMipTransitions==0);
    bounded.upload(0,3,copies); // tracked images still record each region at the cap
    check(bounded.statistics().untrackedImages==3
        &&bounded.statistics().uploadMipTransitions==1);
    bounded.upload(2048,3,copies);
    check(bounded.statistics().untrackedImages==6);
    bounded.forgetImage(0);bounded.upload(2048,3,copies);
    check(bounded.statistics().untrackedImages==6
        &&bounded.statistics().uploadMipTransitions==2);
    copies[0].imageSubresource.mipLevel=2;
    copies[1].imageSubresource.mipLevel=4;
    copies[2].imageSubresource.mipLevel=1;
    bounded.upload(2048,3,copies);
    check(bounded.statistics().uploadMipTransitions==4);
    bounded.forgetImage(2048);bounded.upload(2048,3,copies);
    check(bounded.statistics().uploadMipTransitions==6
        &&bounded.statistics().untrackedImages==6&&bounded.statistics().uploadRegions==1042);
}

void duplicateRetryObservations() {
    using namespace kharvox;
    auto source=std::make_unique<VtPageList>();
    source->count=3;source->pages[0]=7;source->pages[1]=7;source->pages[2]=8;
    VtAppendDiagnostics vt;
    vt.observe(1,*source,vtPageCapacity,{true,0,3},100);
    check(vt.pendingCount()==2);
    vt.observe(1,*source,vtPageCapacity,{true,0,3},150);
    check(vt.pendingCount()==2);
    // The first accepted 7 resolves its old age; its deferred duplicate starts
    // a new age. An empty-map shortcut must still process the deferred suffix.
    vt.observe(1,*source,vtPageCapacity-1,{true,1,2},200);
    check(vt.pendingCount()==2&&vt.statistics().observedRetries==1
        &&vt.statistics().retryWaitMs==100);
    vt.observe(2,*source,0,{true,3,0},250);
    check(vt.pendingCount()==2&&vt.statistics().observedRetries==1);
    vt.observe(1,*source,0,{true,3,0},300);
    check(vt.pendingCount()==0&&vt.statistics().observedRetries==3
        &&vt.statistics().retryWaitMs==400&&vt.statistics().maxRetryWaitMs==200);
    // Resolve the only pending token before encountering its deferred copy.
    source->count=1;
    vt.observe(1,*source,vtPageCapacity,{true,0,1},400);
    source->count=2;
    vt.observe(1,*source,vtPageCapacity-1,{true,1,1},450);
    check(vt.pendingCount()==1&&vt.statistics().observedRetries==4);
    source->count=1;
    vt.observe(1,*source,0,{true,1,0},475);
    check(vt.pendingCount()==0&&vt.statistics().observedRetries==5
        &&vt.statistics().retryWaitMs==475);
    vt.observe(1,*source,vtPageCapacity,{true,0,1},500);
    vt.observe(1,*source,0,{true,1,0},499); // backward clocks cannot underflow
    check(vt.statistics().retryWaitMs==475&&vt.statistics().observedRetries==6);
}

#ifdef _WIN32
void successfulAppendsDoNotReadUnneededPages() {
    using namespace kharvox;
    struct GuardedList {
        VtPageList* value{};
        ~GuardedList(){if(value)VirtualFree(value,0,MEM_RELEASE);}
    } storage;
    storage.value=static_cast<VtPageList*>(VirtualAlloc(nullptr,sizeof(VtPageList),
        MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    check(storage.value!=nullptr);
    auto& source=*new(storage.value)VtPageList{};
    source.count=vtPageCapacity;source.pages[0]=99;
    SYSTEM_INFO system{};GetSystemInfo(&system);
    check(system.dwPageSize>offsetof(VtPageList,pages)&&system.dwPageSize<sizeof(VtPageList));
    DWORD oldProtection{};
    check(VirtualProtect(reinterpret_cast<unsigned char*>(&source)+system.dwPageSize,
        sizeof(VtPageList)-system.dwPageSize,PAGE_NOACCESS,&oldProtection)!=0);
    VtAppendDiagnostics vt;
    // With no pending requests, even a full native append is constant time.
    vt.observe(7,source,0,{true,vtPageCapacity,0},1);
    check(vt.pendingCount()==0&&vt.statistics().requests==vtPageCapacity
        &&vt.statistics().appended==vtPageCapacity&&vt.statistics().destinationPeak==vtPageCapacity);
    VtPageList deferred{};deferred.count=1;deferred.pages[0]=99;
    vt.observe(7,deferred,vtPageCapacity,{true,0,1},10);
    // Resolving the final pending request must skip the remaining accepted
    // prefix. Reading its protected page storage would fail this test.
    vt.observe(7,source,0,{true,vtPageCapacity,0},20);
    check(vt.pendingCount()==0&&vt.statistics().observedRetries==1
        &&vt.statistics().retryWaitMs==10&&vt.statistics().calls==3
        &&vt.statistics().requests==2*vtPageCapacity+1);
}
#endif
}

int main() {
    using namespace kharvox;
    batchedTextureUploads();
    duplicateRetryObservations();
#ifdef _WIN32
    successfulAppendsDoNotReadUnneededPages();
#endif
    auto destination=std::make_unique<VtPageList>();
    auto source=std::make_unique<VtPageList>();
    destination->count=8190;source->count=4;
    for(unsigned i=0;i<4;++i)source->pages[i]=100+i;
    const auto original=*source;
    const auto result=appendVirtualTexturePages(destination.get(),source.get());
    VtAppendDiagnostics vt;
    vt.observe(1,*source,8190,result,100);
    check(vt.statistics().requests==4&&vt.statistics().appended==2
        &&vt.statistics().deferred==2&&vt.pendingCount()==2);
    // A repeated deferral must keep the first timestamp, not reset its age.
    vt.observe(1,*source,8192,{true,0,4},150);
    check(vt.pendingCount()==4);
    vt.observe(1,*source,0,{true,4,0},200);
    check(vt.pendingCount()==0&&vt.statistics().observedRetries==4);
    check(vt.statistics().retryWaitMs==300&&vt.statistics().maxRetryWaitMs==100);
    check(vt.statistics().destinationPeak==8192);
    check(std::memcmp(source.get(),&original,sizeof(original))==0);
    vt.observe(1,*source,0,{false,0,0},300);
    vt.observe(1,*source,8192,{true,1,3},300);
    check(vt.statistics().calls==3); // reject invalid observations
    vt.observe(1,*source,8192,{true,0,4},400);
    vt.observe(2,*source,0,{true,4,0},500); // different VT owner cannot resolve requests
    check(vt.pendingCount()==4&&vt.statistics().observedRetries==4);

    // Diagnostic memory is bounded independently of native lists/capacity.
    source->count=vtPageCapacity;
    for(unsigned i=0;i<vtPageCapacity;++i)source->pages[i]=i+1000;
    vt.observe(1,*source,8192,{true,0,8192},600);
    check(vt.pendingCount()==vtPageCapacity&&vt.statistics().untrackedDeferrals==4);

    TextureQualityDiagnostics textures;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.minFilter=VK_FILTER_LINEAR;sampler.magFilter=VK_FILTER_NEAREST;
    sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.anisotropyEnable=VK_TRUE;sampler.maxAnisotropy=8;
    sampler.mipLodBias=-.5f;sampler.minLod=0;sampler.maxLod=12;
    textures.sampler(sampler);
    sampler.mipLodBias=1;sampler.anisotropyEnable=VK_FALSE;
    sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;textures.sampler(sampler);
    auto s=textures.statistics();
    check(s.samplers==2&&s.linearMin==2&&s.linearMag==0&&s.linearMip==1);
    check(s.anisotropic==1&&s.maximumAnisotropy==8
        &&s.minimumLodBias==-.5f&&s.maximumLodBias==1&&s.maximumSamplerLod==12);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.subresourceRange.baseMipLevel=3;textures.view(view);
    check(textures.statistics().nonzeroBaseMipViews==1
        &&textures.statistics().viewBaseMips[3]==1);
    VkBufferImageCopy copies[3]{};
    copies[0].imageSubresource.mipLevel=4;copies[1].imageSubresource.mipLevel=2;
    copies[2].imageSubresource.mipLevel=2;textures.upload(7,3,copies);
    check(textures.statistics().uploadRegions==3
        &&textures.statistics().uploadedMips[4]==1&&textures.statistics().uploadedMips[2]==2
        &&textures.statistics().uploadMipTransitions==1);
    textures.forgetImage(7);textures.upload(7,1,copies); // reused handle starts fresh
    check(textures.statistics().uploadMipTransitions==1);
    VkImageBlit blit{};blit.dstSubresource.mipLevel=1;
    textures.blit(1,&blit,VK_FILTER_LINEAR);
    check(textures.statistics().blitRegions==1&&textures.statistics().linearBlits==1
        &&textures.statistics().blitDestinationMips[1]==1);
    copies[0].imageSubresource.mipLevel=32;textures.upload(8,1,copies);
    check(textures.statistics().outOfRangeMips==1);
}
