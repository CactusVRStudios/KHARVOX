#include "../src/weapon/MuzzleBranchHook.h"
#include <cassert>
#include <thread>
#include <vector>

int main(){
    const std::array<unsigned char,34> bytes{
        0xb8,11,0,0,0,0x85,0xc9,0x0f,0x85,6,0,0,0,
        0xc3,0x90,0x90,0x90,0x90,0x90,
        0x0f,0x94,0xc2,0x0f,0xb6,0xd2,0xc1,0xe2,8,0x01,0xd0,0x83,0xc0,11,0xc3};
    const auto code=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    assert(code);std::memcpy(code,bytes.data(),bytes.size());
    DWORD previous{};assert(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&previous));
    assert(FlushInstructionCache(GetCurrentProcess(),code,bytes.size()));
    using Function=int(*)(int);
    const auto call=reinterpret_cast<Function>(code);
    assert(call(0)==11&&call(1)==22);
    kharvox::MuzzleBranchHook hook;
    assert(!hook.set(true));
    assert(!hook.install(code+7,{0x0f,0x85,7,0,0,0}));
    assert(call(0)==11&&call(1)==22);
    assert(hook.install(code+7,{0x0f,0x85,6,0,0,0}));
    assert(call(0)==11&&call(1)==22);
    std::array<unsigned char,34> installed{};
    std::memcpy(installed.data(),code,installed.size());
    assert(hook.set(true));assert(!hook.set(true));
    assert(call(0)==278&&call(1)==22);
    assert(hook.set(false));assert(call(0)==11&&call(1)==22);
    std::atomic<bool> start{};
    std::vector<std::thread> workers;
    for(unsigned n=0;n<4;++n)workers.emplace_back([&]{
        while(!start.load(std::memory_order_acquire))std::this_thread::yield();
        for(unsigned i=0;i<100000;++i){
            assert(call(1)==22);
            const auto value=call(0);assert(value==11||value==278);
        }
    });
    start.store(true,std::memory_order_release);
    for(unsigned i=0;i<100000;++i)hook.set(i&1);
    for(auto& worker:workers)worker.join();
    assert(!std::memcmp(installed.data(),code,installed.size()));
    hook.set(false);assert(call(0)==11&&call(1)==22);
    assert(MH_RemoveHook(code+7)==MH_OK);
    assert(!std::memcmp(bytes.data(),code,bytes.size()));
    assert(VirtualFree(code,0,MEM_RELEASE));
}
