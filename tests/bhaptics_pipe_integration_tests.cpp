#include "../src/bhaptics/BhapticsIpcProtocol.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace {

bool writeMessage(HANDLE pipe,
    const kharvox::bhaptics::EncodedIpcMessage& message) {
    DWORD written{};
    return message.size != 0
        && WriteFile(pipe, message.bytes.data(),
            static_cast<DWORD>(message.size), &written, nullptr)
        && written == message.size;
}

HANDLE connect(const std::wstring& path) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (WaitNamedPipeW(path.c_str(), 100)) {
            HANDLE pipe = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE)
                return pipe;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return INVALID_HANDLE_VALUE;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace kharvox::bhaptics;
    if (argc < 3)
        return 1;

    const auto temporary = std::filesystem::temp_directory_path()
        / (L"kharvox-bhaptics-pipe-test-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(temporary, error);
    std::filesystem::create_directories(temporary, error);
    if (error)
        return 2;
    const auto bridge = temporary / L"KharvoxBhapticsBridge.exe";
    const auto sdk = temporary / L"bhaptics_library.dll";
    if (!CopyFileW(argv[1], bridge.c_str(), FALSE)
        || !CopyFileW(argv[2], sdk.c_str(), FALSE))
        return 3;

    const std::wstring pipeName = L"KharvoxBhapticsTest_"
        + std::to_wstring(GetCurrentProcessId());
    const std::wstring pipePath = L"\\\\.\\pipe\\" + pipeName;
    const std::wstring token = L"0123456789abcdef0123456789abcdef";
    const std::string tokenAscii = "0123456789abcdef0123456789abcdef";
    SetEnvironmentVariableW(L"KHARVOX_BHAPTICS_PIPE_NAME", pipeName.c_str());
    SetEnvironmentVariableW(L"KHARVOX_BHAPTICS_SESSION_TOKEN", token.c_str());
    const auto parent = std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"KHARVOX_BHAPTICS_PARENT_PID", parent.c_str());
    SetEnvironmentVariableW(L"KHARVOX_BHAPTICS_APP_ID", nullptr);
    SetEnvironmentVariableW(L"KHARVOX_BHAPTICS_API_KEY", nullptr);

    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + bridge.wstring() + L"\"";
    const BOOL started = CreateProcessW(bridge.c_str(), command.data(), nullptr,
        nullptr, FALSE, CREATE_NO_WINDOW, nullptr, temporary.c_str(),
        &startup, &process);
    SetEnvironmentVariableW(L"KHARVOX_BHAPTICS_PIPE_NAME", nullptr);
    SetEnvironmentVariableW(L"KHARVOX_BHAPTICS_SESSION_TOKEN", nullptr);
    SetEnvironmentVariableW(L"KHARVOX_BHAPTICS_PARENT_PID", nullptr);
    if (!started)
        return 4;
    CloseHandle(process.hThread);

    HANDLE first = connect(pipePath);
    if (first == INVALID_HANDLE_VALUE) {
        TerminateProcess(process.hProcess, 9);
        CloseHandle(process.hProcess);
        return 5;
    }
    if (!writeMessage(first, encodeHello(1, 10, tokenAscii))
        || !writeMessage(first, encodeRumbleState(2, 20, 1000, 2000))) {
        CloseHandle(first);
        TerminateProcess(process.hProcess, 9);
        CloseHandle(process.hProcess);
        return 6;
    }
    CloseHandle(first); // Simulated DOOM/pipe loss.

    HANDLE second = connect(pipePath);
    if (second == INVALID_HANDLE_VALUE) {
        TerminateProcess(process.hProcess, 9);
        CloseHandle(process.hProcess);
        return 7;
    }
    if (!writeMessage(second, encodeHello(1, 30, tokenAscii))
        || !writeMessage(second, encodeEmptyMessage(
            IpcMessageType::Shutdown, 2, 40))) {
        CloseHandle(second);
        TerminateProcess(process.hProcess, 9);
        CloseHandle(process.hProcess);
        return 8;
    }
    CloseHandle(second);

    const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
    DWORD exitCode{};
    if (wait != WAIT_OBJECT_0
        || !GetExitCodeProcess(process.hProcess, &exitCode)
        || exitCode != 0) {
        TerminateProcess(process.hProcess, 9);
        CloseHandle(process.hProcess);
        return 9;
    }
    CloseHandle(process.hProcess);
    std::filesystem::remove_all(temporary, error);
    return 0;
}
