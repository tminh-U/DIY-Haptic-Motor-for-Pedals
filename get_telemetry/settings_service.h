#pragma once

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <string>

struct AppSettings {
    int masterVolume = 78;
    int absPulse = 100;
    int roadTexture = 68;
    int tireSlip = 82;
    bool startWithWindows = false;
    bool minimizeToTray = true;
};

inline std::wstring settingsFilePath() {
    wchar_t localAppData[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                                   SHGFP_TYPE_CURRENT, localAppData))) {
        std::wstring directory = std::wstring(localAppData) + L"\\HapticBrakeControl";
        CreateDirectoryW(directory.c_str(), nullptr);
        return directory + L"\\settings.ini";
    }
    return L"haptic_brake_control.ini";
}

inline int readSettingInt(const std::wstring& file, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(L"Settings", key, fallback, file.c_str()));
}

inline AppSettings loadAppSettings() {
    const std::wstring file = settingsFilePath();
    AppSettings value;
    value.masterVolume = std::clamp(readSettingInt(file, L"MasterVolume", 78), 0, 100);
    value.absPulse = std::clamp(readSettingInt(file, L"AbsPulse", 100), 0, 100);
    value.roadTexture = std::clamp(readSettingInt(file, L"RoadTexture", 68), 0, 100);
    value.tireSlip = std::clamp(readSettingInt(file, L"TireSlip", 82), 0, 100);
    value.startWithWindows = readSettingInt(file, L"StartWithWindows", 0) != 0;
    value.minimizeToTray = readSettingInt(file, L"MinimizeToTray", 1) != 0;
    return value;
}

inline void saveAppSettings(const AppSettings& value) {
    const std::wstring file = settingsFilePath();
    auto writeInt = [&](const wchar_t* key, int number) {
        const std::wstring text = std::to_wstring(number);
        WritePrivateProfileStringW(L"Settings", key, text.c_str(), file.c_str());
    };
    writeInt(L"MasterVolume", value.masterVolume);
    writeInt(L"AbsPulse", value.absPulse);
    writeInt(L"RoadTexture", value.roadTexture);
    writeInt(L"TireSlip", value.tireSlip);
    writeInt(L"StartWithWindows", value.startWithWindows ? 1 : 0);
    writeInt(L"MinimizeToTray", value.minimizeToTray ? 1 : 0);
}

inline bool setStartWithWindows(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    bool success = false;
    if (enabled) {
        wchar_t executable[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, executable, MAX_PATH) > 0) {
            std::wstring command = L"\"" + std::wstring(executable) + L"\" --tray";
            success = RegSetValueExW(key, L"HapticBrakeControl", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        }
    } else {
        const LONG result = RegDeleteValueW(key, L"HapticBrakeControl");
        success = result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(key);
    return success;
}

