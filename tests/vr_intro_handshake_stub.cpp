#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <string>
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    int argc{};auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(argc==4&&std::wstring(argv[1])==L"--vr-intro"){argv[1]=argv[2];argv[2]=argv[3];argc=3;}
    if(argc!=3)return 1;
    std::wstring prefix=L"Local\\KHARVOX-VR-INTRO-"+std::wstring(argv[1]);
    HANDLE dismissed=OpenEventW(EVENT_MODIFY_STATE,FALSE,(prefix+L"-dismissed").c_str());
    HANDLE release=OpenEventW(SYNCHRONIZE,FALSE,(prefix+L"-release").c_str());
    HANDLE released=OpenEventW(EVENT_MODIFY_STATE,FALSE,(prefix+L"-released").c_str());
    HANDLE testDismiss=CreateEventW(nullptr,TRUE,FALSE,(prefix+L"-test-dismiss").c_str());
    wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
    {std::ofstream file(std::filesystem::path(exe).parent_path()/"helper-pid.txt");file<<GetCurrentProcessId();}
    {std::ofstream file(std::filesystem::path(exe).parent_path()/"helper-token.txt");for(auto c:std::wstring(argv[1]))file<<char(c);}
    HANDLE waits[]{testDismiss,release};auto wait=WaitForMultipleObjects(2,waits,FALSE,15000);
    if(wait==WAIT_OBJECT_0){SetEvent(dismissed);WaitForSingleObject(release,15000);}
    if(std::filesystem::exists(std::filesystem::path(exe).parent_path()/"hang-teardown"))Sleep(INFINITE);
    SetEvent(released);
    // Completion notification alone must not open the DOOM launch gate.
    Sleep(750);
    {std::ofstream file(std::filesystem::path(exe).parent_path()/"helper-exited.txt");file<<GetCurrentProcessId();}
    for(auto handle:{dismissed,release,released,testDismiss})CloseHandle(handle);LocalFree(argv);return 0;
}
