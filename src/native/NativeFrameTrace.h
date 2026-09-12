#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <string>
namespace kharvox::native::trace {
bool enabled();
bool active();
void initialize();
void poll();
void beginFrame(uint64_t serial);
void endFrame(uint64_t serial,bool ready);
void presentFinished();
void requestCapture();
bool consumeResourceSnapshot();
void writeOrDefer(const char* name,const std::string& text);
void setEye(uint32_t eye);
uint64_t timestamp();
void row(const char* name,uint64_t link=0,std::array<uint64_t,8> values={});
struct Scope {
 uint64_t id{},start{},frame{},object{},previous{},capture{};uint32_t thread{},eye{};const char* name{};
 int64_t result{};
 explicit Scope(const char* label,uint64_t handle=0);
 ~Scope();
 Scope(const Scope&)=delete;
 Scope& operator=(const Scope&)=delete;
};
struct SubmitScope {
 Scope scope;bool owner{};
 SubmitScope(const char* origin,VkQueue,uint32_t,const VkSubmitInfo*,VkFence);
 SubmitScope(const char* origin,VkQueue,uint32_t,const VkSubmitInfo2*,VkFence);
 ~SubmitScope();
 void result(VkResult result){scope.result=result;}
};
PFN_vkVoidFunction wrap(const char* name,PFN_vkVoidFunction next,bool driver=false);
}
