#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

static std::string reportPath,requestedName,logged;
namespace kharvox {std::string logPathA(const char* name){requestedName=name;return reportPath;}}
namespace kharvoxnative::log {void error(const std::string& message){logged=message;}}
namespace kharvox::native {
static std::atomic_bool outstanding{};
static std::atomic_int frame_root_stage{1};
static thread_local bool inside_real_frame_root{},inside_frame_root_double{true};
namespace cpu {struct Frame{uint64_t serial{55};};static thread_local Frame current;}
static int phase(){return 3;}
[[noreturn]] static void fail(const char* reason){throw std::runtime_error(reason);}
// Execute the production diagnostic with a throwing test termination endpoint.
#include "../src/native/NativeResourceGuard.inc"
}
int main(){
 char directory[MAX_PATH]{},file[MAX_PATH]{};
 assert(GetTempPathA(MAX_PATH,directory));
 assert(GetTempFileNameA(directory,"kvg",0,file));reportPath=file;
 kharvox::native::beforeDestroy("vkDestroyBuffer",0x1234);
 assert(requestedName.empty()&&logged.empty()); // normal path does no diagnostic I/O
 kharvox::native::outstanding=true;
 bool rejected{};
 try{kharvox::native::beforeDestroy("vkDestroyBuffer",0x1234);}
 catch(const std::runtime_error& e){rejected=std::string(e.what())=="resource destruction overlaps an uncompleted native frame";}
 assert(rejected);
 std::ifstream stream(reportPath);std::string saved{std::istreambuf_iterator<char>(stream),{}};stream.close();
 assert(saved==logged&&!saved.empty()); // persisted before termination endpoint
 assert(saved.find("operation=vkDestroyBuffer resource=0x1234")!=std::string::npos);
 assert(saved.find("threadFrame=55")!=std::string::npos);
 assert(saved.find("insideOriginal=false insideDoubled=true")!=std::string::npos);
 assert(saved.find("stack[0]")!=std::string::npos);
 assert(requestedName==std::format("native_resource_guard-{}.log",GetCurrentProcessId()));
 assert(DeleteFileA(reportPath.c_str()));
}
