#include "../src/common/RuntimePaths.h"

#include <string>

int main() {
    const std::wstring directory = kharvox::runtimeDirectory();
    if (directory.empty()) return 1;

    const std::wstring file = kharvox::runtimePath(L"marker.test");
    if (file.size() <= directory.size()) return 2;
    if (file.compare(0, directory.size(), directory) != 0) return 3;
    if (file.substr(file.size() - 11) != L"marker.test") return 4;

    const std::string narrow = kharvox::runtimePathA("marker.test");
    if (narrow.empty() || narrow.find("marker.test") == std::string::npos) return 5;

    wchar_t temp[MAX_PATH]{};
    if(!GetTempPathW(MAX_PATH,temp))return 6;
    if(kharvox::logPath(L"native_stereo.log")!=std::wstring(temp)+L"KHARVOX-NATIVE-STEREO.log")return 7;
    if(kharvox::logPath(L"vr_intro.log")!=std::wstring(temp)+L"KHARVOX-VR-INTRO.log")return 8;
    if(kharvox::logPath(L"native_capture.json")!=std::wstring(temp)+L"KHARVOX-Diagnostics\\native_capture.json")return 9;
    if(kharvox::logPathA("native_stereo.log").find("KHARVOX-NATIVE-STEREO.log")==std::string::npos)return 10;
    return 0;
}
