#pragma once
namespace kharvox::native {
enum class InlineStartupResult { AlreadyInline, Applied, UnsafeBoundary, ReadFailed, UnsupportedValue, WriteFailed };
struct InlineStartup {
 bool attempted{};
 int before{},after{};
 template<class Read,class Write>
 InlineStartupResult initialize(bool beforeRendering,Read read,Write write){
  if(attempted||!beforeRendering)return InlineStartupResult::UnsafeBoundary;
  attempted=true;
  if(!read(before))return InlineStartupResult::ReadFailed;
  after=before;
  if(before==1)return InlineStartupResult::AlreadyInline;
  if(before!=2)return InlineStartupResult::UnsupportedValue;
  if(!write("1"))return InlineStartupResult::WriteFailed;
  if(!read(after))return InlineStartupResult::ReadFailed;
  return after==1?InlineStartupResult::Applied:InlineStartupResult::WriteFailed;
 }
};
}
