#include "../common/DiagnosticLogging.h"
#include "NativeStereo.h"
#include "upstream/include/kharvoxnative/openxr_context.h"
#include "upstream/include/kharvoxnative/log.h"
#include "../common/RuntimePaths.h"
#include <fstream>
#include <mutex>
namespace kharvoxnative {
VulkanState& vulkan_state(){static VulkanState v;return v;}
OpenXRContext& xr(){static OpenXRContext inert;return inert;}
namespace log {
void init(){} void shutdown(){}
void write(std::string_view s){static std::mutex m;std::lock_guard<std::mutex> l(m);std::ofstream f(kharvox::logPathA("native_stereo.log"),std::ios::app);f<<s<<'\n';}
void info(std::string_view s){if(kharvox::extendedDiagnosticsEnabled())write(s);}
void warn(std::string_view s){write(s);} void error(std::string_view s){write(s);}
}}
