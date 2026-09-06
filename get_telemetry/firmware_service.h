#pragma once

#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <string>
#include <vector>

struct FirmwareRelease {
    bool ok = false;
    std::string version;
    std::string binaryUrl;
    std::string sketchUrl;
    DWORD flashOffset = 0;
    bool supportsBlankBoard = false;
    std::string error;
};

inline std::string jsonStringValue(const std::string& json, const std::string& key,
                                   size_t startAt = 0) {
    const std::string marker = "\"" + key + "\"";
    size_t position = json.find(marker, startAt);
    if (position == std::string::npos) return {};
    position = json.find(':', position + marker.size());
    if (position == std::string::npos) return {};
    position = json.find('"', position + 1);
    if (position == std::string::npos) return {};
    ++position;

    std::string result;
    bool escaped = false;
    for (; position < json.size(); ++position) {
        const char value = json[position];
        if (escaped) {
            result.push_back(value == 'n' ? '\n' : value);
            escaped = false;
        } else if (value == '\\') {
            escaped = true;
        } else if (value == '"') {
            break;
        } else {
            result.push_back(value);
        }
    }
    return result;
}

inline FirmwareRelease parseFirmwareReleaseJson(const std::string& response) {
    FirmwareRelease release;
    release.version = jsonStringValue(response, "tag_name");
    std::string firstApplicationBinary;
    size_t assetPosition = 0;
    while ((assetPosition = response.find("\"browser_download_url\"", assetPosition))
            != std::string::npos) {
        std::string url = jsonStringValue(response, "browser_download_url", assetPosition);
        std::string lowered = url;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered.size() >= 4 && lowered.substr(lowered.size() - 4) == ".ino") {
            if (release.sketchUrl.empty()) release.sketchUrl = url;
        } else if (lowered.size() >= 4 && lowered.substr(lowered.size() - 4) == ".bin") {
            if (firstApplicationBinary.empty()) firstApplicationBinary = url;
            const bool fullImage = lowered.find("merged") != std::string::npos
                || lowered.find("factory") != std::string::npos
                || lowered.find("full") != std::string::npos;
            if (fullImage) {
                release.binaryUrl = url;
                release.flashOffset = 0x0;
                release.supportsBlankBoard = true;
                break;
            }
        }
        ++assetPosition;
    }
    if (release.binaryUrl.empty() && !firstApplicationBinary.empty()) {
        release.binaryUrl = firstApplicationBinary;
        release.flashOffset = 0x10000;
    }
    if (release.version.empty()) {
        release.error = "The latest GitHub release has no version tag.";
        return release;
    }
    if (release.version[0] == 'v' || release.version[0] == 'V') {
        release.version.erase(release.version.begin());
    }
    release.ok = true;
    return release;
}

inline FirmwareRelease fetchLatestFirmwareRelease() {
    FirmwareRelease release;
    HINTERNET session = WinHttpOpen(L"HapticBrakeControl/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        release.error = "Could not initialize the update service.";
        return release;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 8000);

    HINTERNET connection = WinHttpConnect(session, L"api.github.com",
                                           INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET",
        L"/repos/tminh-U/DIY-Haptic-Motor-for-Pedals/releases/latest", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;

    const wchar_t* headers = L"Accept: application/vnd.github+json\r\n"
                             L"X-GitHub-Api-Version: 2022-11-28\r\n";
    bool sent = request && WinHttpSendRequest(request, headers, -1L,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (sent) {
        WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
            WINHTTP_NO_HEADER_INDEX);
    }

    std::string response;
    while (sent) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        const size_t oldSize = response.size();
        response.resize(oldSize + available);
        DWORD received = 0;
        if (!WinHttpReadData(request, response.data() + oldSize, available, &received)) {
            sent = false;
            break;
        }
        response.resize(oldSize + received);
    }

    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (!sent || statusCode != 200) {
        release.error = statusCode == 404
            ? "No published GitHub release was found."
            : "GitHub update check failed (HTTP " + std::to_string(statusCode) + ").";
        return release;
    }

    return parseFirmwareReleaseJson(response);
}

inline std::wstring firmwareServiceWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), count);
    result.pop_back();
    return result;
}

inline bool downloadFirmwareBinary(const std::string& url,
                                   const std::wstring& destination,
                                   std::atomic<int>* progress,
                                   std::string& error) {
    if (url.empty()) {
        error = "The latest release does not contain a downloadable firmware asset.";
        return false;
    }

    const std::wstring wideUrl = firmwareServiceWide(url);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    wchar_t extra[2048] = {};
    URL_COMPONENTSW components = {};
    components.dwStructSize = sizeof(components);
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUrlPath = path;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    components.lpszExtraInfo = extra;
    components.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components)) {
        error = "The firmware asset URL is invalid.";
        return false;
    }

    std::wstring resource(path, components.dwUrlPathLength);
    resource.append(extra, components.dwExtraInfoLength);
    HINTERNET session = WinHttpOpen(L"HapticBrakeControl/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = "Could not initialize the firmware download.";
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 15000);
    HINTERNET connection = WinHttpConnect(session, host, components.nPort, 0);
    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS
        ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET",
        resource.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags) : nullptr;
    bool receivedResponse = request && WinHttpSendRequest(request,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr);

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (receivedResponse) {
        WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
            WINHTTP_NO_HEADER_INDEX);
    }

    DWORD totalSize = 0;
    DWORD totalSizeLength = sizeof(totalSize);
    if (receivedResponse) {
        WinHttpQueryHeaders(request,
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &totalSize, &totalSizeLength,
            WINHTTP_NO_HEADER_INDEX);
    }

    HANDLE output = INVALID_HANDLE_VALUE;
    bool success = receivedResponse && statusCode == 200;
    if (success) {
        output = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        success = output != INVALID_HANDLE_VALUE;
    }

    DWORD downloaded = 0;
    if (progress) *progress = 5;
    while (success) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            success = false;
            break;
        }
        if (available == 0) break;
        std::vector<char> buffer(available);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &bytesRead)) {
            success = false;
            break;
        }
        DWORD bytesWritten = 0;
        if (!WriteFile(output, buffer.data(), bytesRead, &bytesWritten, nullptr)
            || bytesWritten != bytesRead) {
            success = false;
            break;
        }
        downloaded += bytesRead;
        if (progress && totalSize > 0) {
            *progress = std::min(95, 5 + static_cast<int>(downloaded * 90ULL / totalSize));
        }
    }

    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (!success) {
        DeleteFileW(destination.c_str());
        if (statusCode != 200) {
            error = "Firmware download failed (HTTP " + std::to_string(statusCode) + ").";
        } else {
            error = "Firmware download was interrupted.";
        }
        return false;
    }
    if (progress) *progress = 100;
    return true;
}

inline std::wstring quoteArgument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    for (wchar_t character : value) {
        if (character == L'\"') quoted += L'\\';
        quoted += character;
    }
    return quoted + L"\"";
}

inline bool firmwareServiceFileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline std::wstring firmwareServiceAppDirectory() {
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring directory(modulePath);
    const size_t slash = directory.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : directory.substr(0, slash);
}

inline std::wstring findArduinoCli() {
    const std::wstring sibling = firmwareServiceAppDirectory() + L"\\arduino-cli.exe";
    if (firmwareServiceFileExists(sibling)) return sibling;

    wchar_t programFiles[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH) > 0) {
        const std::wstring installed = std::wstring(programFiles)
            + L"\\Arduino CLI\\arduino-cli.exe";
        if (firmwareServiceFileExists(installed)) return installed;
    }

    wchar_t resolved[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"arduino-cli.exe", nullptr, MAX_PATH, resolved, nullptr) > 0) {
        return resolved;
    }
    return {};
}

inline bool runFirmwareCommand(const std::wstring& executable,
                               const std::wstring& arguments,
                               DWORD& exitCode, std::string& output,
                               std::string& error) {
    SECURITY_ATTRIBUTES security = {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE pipeRead = nullptr;
    HANDLE pipeWrite = nullptr;
    if (!CreatePipe(&pipeRead, &pipeWrite, &security, 0)
        || !SetHandleInformation(pipeRead, HANDLE_FLAG_INHERIT, 0)) {
        if (pipeRead) CloseHandle(pipeRead);
        if (pipeWrite) CloseHandle(pipeWrite);
        error = "Could not create the Arduino CLI output pipe.";
        return false;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = pipeWrite;
    startup.hStdError = pipeWrite;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process = {};
    std::wstring command = quoteArgument(executable) + L" " + arguments;
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    const std::wstring workingDirectory = firmwareServiceAppDirectory();
    const BOOL started = CreateProcessW(executable.c_str(), mutableCommand.data(),
        nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        workingDirectory.c_str(), &startup, &process);
    CloseHandle(pipeWrite);
    if (!started) {
        CloseHandle(pipeRead);
        error = "Could not start Arduino CLI (Windows error "
            + std::to_string(GetLastError()) + ").";
        return false;
    }

    char buffer[2048];
    DWORD bytesRead = 0;
    while (ReadFile(pipeRead, buffer, sizeof(buffer), &bytesRead, nullptr)
           && bytesRead > 0) {
        output.append(buffer, bytesRead);
        if (output.size() > 32768) output.erase(0, output.size() - 32768);
    }
    CloseHandle(pipeRead);
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0) {
        error = "Arduino CLI failed with exit code " + std::to_string(exitCode) + ".";
        if (!output.empty()) {
            const size_t start = output.size() > 1200 ? output.size() - 1200 : 0;
            error += " " + output.substr(start);
        }
        return false;
    }
    return true;
}

inline bool ensureEsp32ArduinoCore(const std::wstring& cli, std::string& error) {
    DWORD exitCode = 0;
    std::string output;
    if (runFirmwareCommand(cli, L"core list --format json", exitCode, output, error)
        && output.find("esp32:esp32") != std::string::npos) {
        return true;
    }

    output.clear();
    error.clear();
    const std::wstring indexUrl =
        L"https://espressif.github.io/arduino-esp32/package_esp32_index.json";
    return runFirmwareCommand(cli,
        L"core install esp32:esp32 --additional-urls " + quoteArgument(indexUrl),
        exitCode, output, error);
}

inline bool compileAndUploadSketchWithArduinoCli(const std::wstring& cli,
                                                 const std::wstring& port,
                                                 const std::wstring& sketchPath,
                                                 DWORD& exitCode,
                                                 std::string& error) {
    std::string output;
    error.clear();
    const std::wstring arguments = L"compile --fqbn esp32:esp32:esp32 --upload --port "
        + quoteArgument(port) + L" " + quoteArgument(sketchPath);
    return runFirmwareCommand(cli, arguments, exitCode, output, error);
}

inline bool flashFirmwareWithEsptool(const std::wstring& port,
                                     const std::wstring& firmwarePath,
                                     DWORD flashOffset,
                                     DWORD& exitCode, std::string& error) {
    const std::wstring directory = firmwareServiceAppDirectory();

    std::wstring tool = directory + L"\\esptool.exe";
    if (GetFileAttributesW(tool.c_str()) == INVALID_FILE_ATTRIBUTES) tool = L"esptool.exe";
    wchar_t offsetText[16] = {};
    swprintf_s(offsetText, L"0x%lX", static_cast<unsigned long>(flashOffset));
    std::wstring command = quoteArgument(tool) + L" --chip esp32 --port "
        + quoteArgument(port) + L" --baud 460800 write_flash " + offsetText + L" "
        + quoteArgument(firmwarePath);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process)) {
        error = "Could not start esptool.exe. Put it next to the app or add it to PATH.";
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0) {
        error = "esptool failed with exit code " + std::to_string(exitCode) + ".";
        return false;
    }
    return true;
}
