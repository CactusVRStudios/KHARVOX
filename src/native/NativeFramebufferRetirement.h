#pragma once
#include <algorithm>
#include <vector>

namespace kharvox::native {
// A source view may die before its unused source framebuffer. Invalidate every
// dependent mirror and its source metadata now, before the view handle is reused.
// The caller owns the metadata locks and retires returned GPU handles after idle.
template<class View, class Infos, class Targets, class References, class Forget>
auto detachFramebuffersUsingView(View view, Infos& infos, Targets& targets,
                                References& references, Forget forget,
                                bool sourceViewDestroyed=true) {
 using Framebuffer = decltype(typename Targets::mapped_type{}.framebuffer);
 std::vector<Framebuffer> retired;
 for(auto it=infos.begin();it!=infos.end();) {
  const auto& attachments=it->second.attachments;
  if(std::find(attachments.begin(),attachments.end(),view)==attachments.end()){++it;continue;}
  if(auto target=targets.find(it->first);target!=targets.end()) {
   if(target->second.framebuffer) {
    retired.push_back(target->second.framebuffer);
    for(auto attachment:attachments)
     if(auto ref=references.find(attachment);ref!=references.end())ref->second.remove(true);
   }
   targets.erase(target);
  }
  if(sourceViewDestroyed){forget(it->first);it=infos.erase(it);}
  else ++it; // A surviving source alias can rebuild its mirror from this metadata.
 }
 return retired;
}
}
