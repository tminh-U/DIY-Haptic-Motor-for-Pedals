#pragma once

#include <windows.h>
#include <string>
#include "settings_service.h"

namespace modern_ui {

constexpr COLORREF Background = RGB(7, 22, 34);
constexpr COLORREF Header = RGB(8, 25, 38);
constexpr COLORREF Surface = RGB(11, 31, 46);
constexpr COLORREF SurfaceRaised = RGB(14, 38, 56);
constexpr COLORREF Border = RGB(35, 65, 88);
constexpr COLORREF Blue = RGB(24, 150, 255);
constexpr COLORREF Cyan = RGB(24, 190, 246);
constexpr COLORREF Text = RGB(242, 247, 255);
constexpr COLORREF Muted = RGB(160, 184, 218);
constexpr COLORREF Track = RGB(48, 68, 94);
constexpr COLORREF Amber = RGB(255, 190, 20);
constexpr COLORREF Green = RGB(38, 211, 125);
constexpr COLORREF Red = RGB(255, 55, 50);

struct Fonts {
    HFONT regular = nullptr;
    HFONT semibold = nullptr;
    HFONT small = nullptr;
    HFONT pageTitle = nullptr;
    HFONT hero = nullptr;
    HFONT value = nullptr;
};

struct DrawModel {
    int page = 0;
    AppSettings settings;
    bool connected = false;
    bool panicked = false;
    bool checkingUpdate = false;
    bool flashing = false;
    bool portSelected = false;
    bool binaryAvailable = false;
    bool manualBinaryAvailable = false;
    bool gameDetected = false;
    bool telemetryActive = false;
    int firmwareStage = 0; // 0 idle, 1 downloading, 2 binary flash, 3 CLI setup, 4 compile/upload
    int downloadProgress = 0;
    std::wstring deviceId;
    std::wstring currentFirmware = L"Not detected";
    std::wstring latestFirmware = L"Not checked";
    std::wstring firmwareStatus = L"Ready";
    std::wstring selectedPort = L"No COM ports found";
    std::wstring gameName = L"No game detected";
    std::wstring gameStatus = L"Start Assetto Corsa or ACC";
};

inline RECT rect(int left, int top, int right, int bottom) {
    return RECT{left, top, right, bottom};
}

inline bool contains(const RECT& value, int x, int y) {
    return x >= value.left && x < value.right && y >= value.top && y < value.bottom;
}

inline void fill(HDC dc, const RECT& area, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &area, brush);
    DeleteObject(brush);
}

inline void rounded(HDC dc, const RECT& area, COLORREF background, COLORREF border,
                    int radius = 14) {
    HBRUSH brush = CreateSolidBrush(background);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, area.left, area.top, area.right, area.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

inline void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int width = 1) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, old);
    DeleteObject(pen);
}

inline void text(HDC dc, const std::wstring& value, RECT area, HFONT font,
                 COLORREF color, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, value.c_str(), -1, &area, format | DT_NOPREFIX);
    SelectObject(dc, old);
}

inline void circle(HDC dc, int centerX, int centerY, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, centerX - radius, centerY - radius, centerX + radius, centerY + radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

inline void drawWaveLogo(HDC dc, int x, int y) {
    const int heights[] = {8, 24, 38, 20, 10};
    for (int index = 0; index < 5; ++index) {
        const int px = x + index * 8;
        line(dc, px, y - heights[index] / 2, px, y + heights[index] / 2, Cyan, 3);
    }
}

inline void drawButton(HDC dc, RECT area, const std::wstring& label, const Fonts& fonts,
                       bool enabled = true, bool primary = true) {
    COLORREF bg = enabled ? (primary ? RGB(9, 116, 241) : RGB(24, 54, 91)) : RGB(35, 51, 67);
    COLORREF edge = enabled ? (primary ? RGB(30, 161, 255) : RGB(53, 91, 139)) : RGB(48, 61, 74);
    rounded(dc, area, bg, edge, 12);
    text(dc, label, area, fonts.semibold, enabled ? Text : RGB(102, 120, 138),
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

inline void drawHeader(HDC dc, const DrawModel& model, const Fonts& fonts) {
    fill(dc, rect(0, 0, 720, 72), Header);
    drawWaveLogo(dc, 30, 36);
    text(dc, L"Haptic Brake Control", rect(85, 8, 500, 64), fonts.pageTitle, Text);

    line(dc, 624, 36, 648, 36, Muted, 2);
    line(dc, 678, 25, 697, 46, Muted, 2);
    line(dc, 697, 25, 678, 46, Muted, 2);
    line(dc, 0, 71, 720, 71, Border);

    static const wchar_t* labels[] = {
        L"Main", L"Effects", L"Firmware", L"Settings"
    };
    for (int index = 0; index < 4; ++index) {
        RECT tab = rect(index * 180, 72, (index + 1) * 180, 132);
        const COLORREF color = model.page == index ? Cyan : Muted;
        text(dc, labels[index], tab, fonts.small, color,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (model.page == index) {
            rounded(dc, rect(tab.left + 14, 127, tab.right - 14, 132), Blue, Blue, 5);
        }
        if (index != 3) line(dc, tab.right, 88, tab.right, 116, RGB(27, 50, 69));
    }
    line(dc, 0, 132, 720, 132, Border);
}

inline void drawFooter(HDC dc, const Fonts& fonts) {
    line(dc, 28, 850, 692, 850, Border);
    text(dc, L"Fine-tune your haptic feedback experience.", rect(30, 856, 560, 892),
         fonts.small, Muted);
}

inline void drawBoard(HDC dc, const Fonts& fonts) {
    RECT board = rect(70, 205, 310, 730);
    rounded(dc, board, RGB(18, 22, 24), RGB(83, 90, 96), 22);

    for (int y = 230; y < 710; y += 35) {
        circle(dc, 82, y, 5, RGB(219, 167, 75));
        circle(dc, 298, y, 5, RGB(219, 167, 75));
        circle(dc, 82, y, 2, RGB(33, 36, 38));
        circle(dc, 298, y, 2, RGB(33, 36, 38));
    }
    rounded(dc, rect(145, 205, 235, 260), RGB(195, 201, 204), RGB(236, 239, 241), 7);
    rounded(dc, rect(157, 212, 223, 250), RGB(94, 99, 103), RGB(219, 224, 227), 5);
    rounded(dc, rect(128, 330, 252, 460), RGB(8, 10, 12), RGB(70, 76, 80), 8);
    for (int y = 342; y < 454; y += 14) {
        line(dc, 116, y, 128, y, RGB(200, 188, 150), 2);
        line(dc, 252, y, 264, y, RGB(200, 188, 150), 2);
    }
    text(dc, L"HAPTIC", rect(130, 470, 260, 500), fonts.semibold, Text, DT_CENTER);
    text(dc, L"BRAKE", rect(130, 495, 260, 525), fonts.semibold, Text, DT_CENTER);
    text(dc, L"CTRL", rect(130, 520, 260, 550), fonts.semibold, Text, DT_CENTER);
    rounded(dc, rect(105, 630, 275, 700), RGB(35, 115, 61), RGB(86, 187, 105), 6);
    for (int x = 125; x <= 255; x += 43) {
        circle(dc, x, 657, 11, RGB(170, 176, 170));
        line(dc, x - 7, 657, x + 7, 657, RGB(60, 64, 62), 2);
    }
}

inline void drawDevicePage(HDC dc, const DrawModel& model, const Fonts& fonts) {
    drawBoard(dc, fonts);
    text(dc, L"Haptic Brake Control", rect(355, 220, 685, 275), fonts.hero, Text);
    text(dc, L"Haptic feedback for", rect(355, 278, 680, 312), fonts.regular, Muted);
    text(dc, L"a more immersive drive.", rect(355, 312, 690, 348), fonts.regular, Muted);

    RECT status = rect(350, 378, 678, 555);
    rounded(dc, status, SurfaceRaised, Border, 14);
    text(dc, L"Device Status", rect(382, 390, 650, 425), fonts.small, Muted);
    circle(dc, 385, 458, 13, model.connected ? Green : Red);
    text(dc, model.connected ? L"Connected" : L"Disconnected",
         rect(415, 430, 650, 485), fonts.pageTitle, Text);
    std::wstring details;
    if (model.panicked) {
        details = L"Kernel panic detected - reset the ESP32.";
    } else if (model.connected) {
        details = model.deviceId.empty() ? L"Haptic controller ready."
                                         : L"ID  " + model.deviceId;
    } else {
        details = L"Scanning for controller...";
    }
    text(dc, details, rect(375, 492, 662, 536), fonts.small,
         model.panicked ? Red : Muted,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    rounded(dc, rect(350, 575, 678, 680), SurfaceRaised, Border, 14);
    text(dc, L"Game Status", rect(382, 582, 650, 612), fonts.small, Muted);
    circle(dc, 385, 643, 10,
           model.telemetryActive ? Green : (model.gameDetected ? Amber : Track));
    text(dc, model.gameName, rect(415, 610, 660, 643), fonts.semibold, Text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    text(dc, model.gameStatus, rect(415, 642, 660, 672), fonts.small,
         model.telemetryActive ? Green : Muted,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    drawButton(dc, rect(350, 705, 678, 785), L"Firmware Update   >", fonts);
}

inline void drawSlider(HDC dc, int y, int value, bool master, const Fonts& fonts) {
    const int left = 58;
    const int right = 657;
    const int width = right - left;
    rounded(dc, rect(left, y - 5, right, y + 6), Track, Track, 10);
    if (master) {
        const int warningX = left + width * 80 / 100;
        rounded(dc, rect(warningX, y - 5, right, y + 6), RGB(120, 83, 9), RGB(120, 83, 9), 10);
        line(dc, warningX, y - 13, warningX, y + 16, Amber, 2);
        text(dc, L"80%", rect(warningX - 25, y + 12, warningX + 35, y + 39),
             fonts.small, Amber, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    const int fillRight = left + width * value / 100;
    if (fillRight > left) {
        rounded(dc, rect(left, y - 5, fillRight, y + 6), Blue, Blue, 10);
    }
}

inline void drawEffectCard(HDC dc, int top, const wchar_t* titleValue,
                           const wchar_t* description, int value, bool master,
                           const Fonts& fonts) {
    rounded(dc, rect(28, top, 692, top + 150), Surface, Border, 14);
    text(dc, titleValue, rect(55, top + 16, 400, top + 52), fonts.pageTitle, Text);
    text(dc, description, rect(55, top + 48, 510, top + 77), fonts.small, Muted);
    if (master) {
        rounded(dc, rect(365, top + 18, 555, top + 54), RGB(54, 44, 15), RGB(128, 93, 8), 9);
        text(dc, L"!  Warning from 80%", rect(372, top + 18, 548, top + 54),
             fonts.small, Amber, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    rounded(dc, rect(572, top + 16, 665, top + 58), SurfaceRaised, Border, 9);
    text(dc, std::to_wstring(value) + L"%", rect(575, top + 16, 662, top + 58),
         fonts.value, value >= 80 && master ? Amber : Cyan,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    drawSlider(dc, top + 108, value, master, fonts);
}

inline void drawEffectsPage(HDC dc, const DrawModel& model, const Fonts& fonts) {
    drawEffectCard(dc, 152, L"Master Volume", L"Overall haptic output level",
                   model.settings.masterVolume, true, fonts);
    drawEffectCard(dc, 314, L"ABS Pulse", L"Sharp brake-pressure pulse feedback",
                   model.settings.absPulse, false, fonts);
    drawEffectCard(dc, 476, L"Road Texture", L"Kerbs, bumps and suspension movement",
                   model.settings.roadTexture, false, fonts);
    drawEffectCard(dc, 638, L"Tire Slip", L"Front tire lock-up / longitudinal slip",
                   model.settings.tireSlip, false, fonts);
}

inline void drawFirmwarePage(HDC dc, const DrawModel& model, const Fonts& fonts) {
    text(dc, L"Firmware Tools", rect(30, 145, 500, 190), fonts.pageTitle, Text);
    text(dc, L"Update or recover your ESP32 controller.", rect(30, 180, 680, 212),
         fonts.small, Muted);

    rounded(dc, rect(28, 215, 692, 410), Surface, Border, 14);
    text(dc, L"Check for Update", rect(55, 225, 450, 270), fonts.pageTitle, Text);
    text(dc, L"Compare the connected device with the latest GitHub release.",
         rect(55, 260, 665, 290), fonts.small, Muted);
    rounded(dc, rect(55, 295, 665, 350), SurfaceRaised, Border, 9);
    text(dc, L"Device firmware", rect(80, 298, 310, 323), fonts.small, Muted);
    text(dc, model.currentFirmware, rect(80, 320, 325, 348), fonts.value, Cyan,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    line(dc, 355, 305, 355, 340, Border);
    text(dc, L"Latest release", rect(385, 298, 630, 323), fonts.small, Muted);
    text(dc, model.latestFirmware, rect(385, 320, 645, 348), fonts.value, Cyan,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const bool firmwareError = model.firmwareStatus.find(L"failed") != std::wstring::npos
        || model.firmwareStatus.find(L"no flashable") != std::wstring::npos;
    text(dc, model.firmwareStatus, rect(55, 357, 375, 400), fonts.small,
         firmwareError ? Red : Muted,
         DT_LEFT | DT_VCENTER | DT_WORDBREAK);
    drawButton(dc, rect(385, 360, 510, 397),
               model.checkingUpdate ? L"Checking..." : L"Check", fonts,
               !model.checkingUpdate);
    const bool canUpdate = model.connected && !model.checkingUpdate && !model.flashing
        && (model.binaryAvailable || model.latestFirmware == L"Not checked");
    drawButton(dc, rect(520, 360, 665, 397),
               model.flashing ? L"Working..." : L"Update",
               fonts, canUpdate);

    rounded(dc, rect(28, 425, 692, 820), Surface, Border, 14);
    text(dc, L"Manual Flash", rect(55, 438, 400, 480), fonts.pageTitle, Text);
    text(dc, L"For blank ESP32 boards or recovery flashing.",
         rect(55, 470, 665, 505), fonts.small, Muted,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    rounded(dc, rect(55, 510, 665, 560), RGB(47, 43, 20), RGB(145, 103, 0), 9);
    text(dc, L"!   Select the correct COM port before flashing.",
         rect(75, 514, 645, 555), fonts.small, Amber,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    text(dc, L"COM port", rect(55, 565, 300, 590), fonts.small, Muted);
    rounded(dc, rect(55, 594, 665, 637), SurfaceRaised, Border, 9);
    text(dc, model.selectedPort, rect(70, 597, 620, 634), fonts.regular,
         model.portSelected ? Text : Muted);
    line(dc, 635, 609, 644, 618, Muted, 2);
    line(dc, 644, 618, 653, 609, Muted, 2);
    std::wstring operationText = L"Choose COM, then download and flash the latest GitHub release.";
    if (model.firmwareStage == 1) {
        operationText = L"Downloading firmware... " + std::to_wstring(model.downloadProgress) + L"%";
    } else if (model.firmwareStage == 2) {
        operationText = L"Flashing firmware... Do not disconnect USB.";
    } else if (model.firmwareStage == 3) {
        operationText = L"Preparing Arduino CLI and ESP32 core (first run may take a while)...";
    } else if (model.firmwareStage == 4) {
        operationText = L"Compiling and uploading .ino... Do not disconnect USB.";
    }
    text(dc, operationText, rect(55, 655, 665, 718), fonts.small,
         model.flashing ? Amber : Muted,
         DT_LEFT | DT_VCENTER | DT_WORDBREAK);
    const bool releaseCanBeLoaded = model.manualBinaryAvailable
        || model.latestFirmware == L"Not checked";
    const bool canFlash = !model.flashing && !model.checkingUpdate && model.portSelected
        && releaseCanBeLoaded;
    drawButton(dc, rect(385, 745, 665, 805),
               model.flashing ? L"Working..." : L"Download & Flash Latest",
               fonts, canFlash);
}

inline void drawToggle(HDC dc, int centerX, int centerY, bool enabled) {
    rounded(dc, rect(centerX - 38, centerY - 20, centerX + 38, centerY + 20),
            enabled ? RGB(5, 120, 242) : RGB(48, 66, 84),
            enabled ? RGB(27, 157, 255) : RGB(66, 84, 101), 40);
    circle(dc, enabled ? centerX + 18 : centerX - 18, centerY, 15, Text);
}

inline void drawSettingsPage(HDC dc, const DrawModel& model, const Fonts& fonts) {
    text(dc, L"Settings", rect(30, 155, 500, 205), fonts.pageTitle, Text);
    text(dc, L"App behavior and useful links.", rect(30, 195, 670, 230), fonts.small, Muted);

    rounded(dc, rect(28, 245, 692, 355), Surface, Border, 14);
    text(dc, L"Start with Windows", rect(55, 260, 530, 300), fonts.semibold, Text);
    text(dc, L"Launch the app automatically when Windows starts.",
         rect(55, 296, 560, 330), fonts.small, Muted);
    drawToggle(dc, 625, 298, model.settings.startWithWindows);

    rounded(dc, rect(28, 370, 692, 480), Surface, Border, 14);
    text(dc, L"Minimize app to tray when close", rect(55, 385, 550, 425),
         fonts.semibold, Text);
    text(dc, L"Keep the app running in the system tray instead of exiting.",
         rect(55, 421, 570, 455), fonts.small, Muted);
    drawToggle(dc, 625, 423, model.settings.minimizeToTray);

    rounded(dc, rect(28, 495, 692, 625), Surface, Border, 14);
    text(dc, L"GitHub", rect(55, 510, 400, 548), fonts.semibold, Text);
    text(dc, L"Open the project repository for source code, releases, and documentation.",
         rect(55, 548, 440, 608), fonts.small, Muted,
         DT_LEFT | DT_VCENTER | DT_WORDBREAK);
    drawButton(dc, rect(485, 535, 665, 585), L"Open GitHub", fonts, true, false);
}

inline void draw(HDC dc, const DrawModel& model, const Fonts& fonts) {
    fill(dc, rect(0, 0, 720, 900), Background);
    drawHeader(dc, model, fonts);
    if (model.page == 0) drawDevicePage(dc, model, fonts);
    else if (model.page == 1) drawEffectsPage(dc, model, fonts);
    else if (model.page == 2) drawFirmwarePage(dc, model, fonts);
    else drawSettingsPage(dc, model, fonts);
    drawFooter(dc, fonts);
}

} // namespace modern_ui
