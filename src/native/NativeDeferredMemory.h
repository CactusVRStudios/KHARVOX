#pragma once
#include <array>
#include <cstddef>
#include <mutex>

namespace kharvox::native {
// The owner calls complete only after CPU replay retirement and verified GPU
// completion. The lock also excludes a new frame while a physical free runs.
template<class Request, std::size_t Capacity = 128>
class DeferredMemoryFreeQueue {
public:
 enum class Result { Released, Deferred, Refused };
 template<class Start> bool begin(Start start) {
  std::lock_guard lock(mutex_);
  if(active_)return false;
  active_=true;start();return true;
 }
 template<class Release> Result release(const Request& request,bool deferrable,Release physicalFree) {
  std::lock_guard lock(mutex_);
  if(!active_){physicalFree(request);return Result::Released;}
  if(!deferrable||size_==Capacity)return Result::Refused;
  for(std::size_t i=0;i<size_;++i)if(pending_[i]==request)return Result::Refused;
  pending_[size_++]=request;return Result::Deferred;
 }
 template<class Release,class Retire> std::size_t complete(Release physicalFree,Retire retire) {
  std::lock_guard lock(mutex_);
  const auto count=size_;
  for(std::size_t i=0;i<count;++i){physicalFree(pending_[i]);pending_[i]={};}
  size_=0;active_=false;retire();return count;
 }
private:
 std::mutex mutex_;
 std::array<Request,Capacity> pending_{};
 std::size_t size_{};
 bool active_{};
};
}
