/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_WIIU

/* The system software keyboard.

   The structure, and the way the keyboard is created, run and drawn, follow the
   SDL2 Wii U port (devkitPro wut-packages, SDL2-2.32.10.patch,
   src/video/wiiu/SDL_wiiuswkbd.cpp, Copyright (C) 2025 Daniel K. O.
   <dkosmari@gmail.com>, same license). It is rewritten for SDL3: no SDL_syswm
   events, no SDL_WiiUSetSWKBD* calls (the options come in as a plain struct that
   the caller fills from the text input properties), and no Wii Remote input.

   nn::swkbd needs: the FS library and an FS client of its own, a work memory
   block, and a call to Create() after GX2 is initialized. Each frame, Calc()
   takes the controller state; Draw*() draws into the color buffer that is bound
   in the current GX2 context. The caller does all of that around these functions.
*/

#include <coreinit/filesystem.h>
#include <coreinit/mcp.h>
#include <coreinit/userconfig.h>
#include <nn/swkbd.h>

#include "SDL_wiiuswkbd.h"

namespace
{

using nn::swkbd::KeyboardLayout;
using nn::swkbd::LanguageType;
using nn::swkbd::RegionType;

struct SwkbdState
{
    bool fs_initialized = false;
    FSClient *fs_client = nullptr;
    bool fs_client_added = false;
    void *work_memory = nullptr;
    bool created = false;
    RegionType region = RegionType::Europe;
    LanguageType language = LanguageType::English;

    // The strings given to AppearInputForm(): kept until the keyboard is hidden,
    // in case it uses them after the call.
    char16_t *initial_text = nullptr;
    char16_t *hint_text = nullptr;
    char16_t *ok_label = nullptr;

    bool shown = false;   // Show() was called and the keyboard has not finished fading out
    bool closing = false; // Hide() was called, or a result was handled
    int frames_hidden = 0; // Update() calls in which the state was still Hidden after Show()
};

/* The state may still read Hidden for a moment after AppearInputForm(): wait this many
   frames for it to change before deciding the keyboard never appeared. */
constexpr int kMaxHiddenFrames = 30;
SwkbdState s;

char16_t *ToUTF16(const char *text)
{
    if (!text) {
        return nullptr;
    }
    // This CPU is big endian, so UTF-16BE is also the native char16_t
    return reinterpret_cast<char16_t *>(SDL_iconv_string("UTF-16BE", "UTF-8", text, SDL_strlen(text) + 1));
}

char *ToUTF8(const char16_t *text)
{
    size_t length = 0;

    if (!text) {
        return nullptr;
    }
    while (text[length]) {
        ++length;
    }
    return SDL_iconv_string("UTF-8", "UTF-16BE", reinterpret_cast<const char *>(text), 2 * (length + 1));
}

void FreeStrings()
{
    SDL_free(s.initial_text);
    SDL_free(s.hint_text);
    SDL_free(s.ok_label);
    s.initial_text = s.hint_text = s.ok_label = nullptr;
}

// The UC (user config) and MCP calls are the ones the SDL2 port uses.
uint32_t ReadSystemConfigU32(const char *key, uint32_t fallback)
{
    UCHandle handle = UCOpen();
    unsigned result = 0;
    alignas(0x40) UCSysConfig config {};

    if (handle < 0) {
        return fallback;
    }
    SDL_strlcpy(config.name, key, sizeof(config.name));
    config.dataType = UC_DATATYPE_UNSIGNED_INT;
    config.dataSize = sizeof(result);
    config.data = &result;
    UCError status = UCReadSysConfig(handle, 1, &config);
    UCClose(handle);
    return (status == UC_ERROR_OK) ? result : fallback;
}

RegionType ReadSystemRegion()
{
    alignas(0x40) MCPSysProdSettings settings {};
    int handle = MCP_Open();

    if (handle < 0) {
        return RegionType::Europe;
    }
    MCPError status = MCP_GetSysProdSettings(handle, &settings);
    MCP_Close(handle);
    if (status) {
        return RegionType::Europe;
    }
    if (settings.product_area & MCP_REGION_JAPAN) {
        return RegionType::Japan;
    }
    if (settings.product_area & MCP_REGION_USA) {
        return RegionType::USA;
    }
    if (settings.product_area & MCP_REGION_EUROPE) {
        return RegionType::Europe;
    }
    if (settings.product_area & MCP_REGION_CHINA) {
        return RegionType::China;
    }
    if (settings.product_area & MCP_REGION_KOREA) {
        return RegionType::Korea;
    }
    if (settings.product_area & MCP_REGION_TAIWAN) {
        return RegionType::Taiwan;
    }
    return RegionType::Europe;
}

KeyboardLayout DefaultLayout(RegionType region, LanguageType language)
{
    switch (region) {
    case RegionType::Japan:
        return KeyboardLayout::JPN_JP_QWERTY;
    case RegionType::USA:
        switch (language) {
        case LanguageType::English:
            return KeyboardLayout::USA_EN_QWERTY;
        case LanguageType::French:
            return KeyboardLayout::USA_FR_QWERTY;
        case LanguageType::Spanish:
            return KeyboardLayout::USA_ES_QWERTY;
        case LanguageType::Portuguese:
            return KeyboardLayout::USA_PT_QWERTY;
        default:
            return KeyboardLayout::LanguageDefault;
        }
    case RegionType::Europe:
        switch (language) {
        case LanguageType::English:
            return KeyboardLayout::EUR_EN_QWERTY;
        case LanguageType::French:
            return KeyboardLayout::EUR_FR_AZERTY;
        case LanguageType::German:
            return KeyboardLayout::EUR_DE_QWERTZ;
        case LanguageType::Italian:
            return KeyboardLayout::EUR_IT_QWERTY;
        case LanguageType::Spanish:
            return KeyboardLayout::EUR_ES_QWERTY;
        case LanguageType::Dutch:
            return KeyboardLayout::EUR_NL_QWERTY;
        case LanguageType::Portuguese:
            return KeyboardLayout::EUR_PT_QWERTY;
        case LanguageType::Russian:
            return KeyboardLayout::EUR_RU_JCUKEN;
        default:
            return KeyboardLayout::LanguageDefault;
        }
    default:
        return KeyboardLayout::LanguageDefault;
    }
}

void ReleaseCreateResources()
{
    if (s.fs_client) {
        if (s.fs_client_added) {
            FSDelClient(s.fs_client, FS_ERROR_FLAG_NONE);
            s.fs_client_added = false;
        }
        SDL_aligned_free(s.fs_client);
        s.fs_client = nullptr;
    }
    if (s.work_memory) {
        SDL_aligned_free(s.work_memory);
        s.work_memory = nullptr;
    }
    if (s.fs_initialized) {
        FSShutdown();
        s.fs_initialized = false;
    }
}

bool Create()
{
    nn::swkbd::CreateArg arg;
    uint32_t language;
    size_t work_size;

    if (s.created) {
        return true;
    }

    // The language and region come from the system settings. nn::swkbd wants a
    // region that goes with the language, and Chinese and Korean (and the
    // regions they go with) are known to be a problem in the SDL2 port (it
    // left them out), so those fall back to English in the USA region.
    s.region = ReadSystemRegion();
    language = ReadSystemConfigU32("cafe.language", static_cast<uint32_t>(LanguageType::English));
    s.language = (language <= 11) ? static_cast<LanguageType>(language) : LanguageType::English;
    if (s.region == RegionType::China || s.region == RegionType::Korea || s.region == RegionType::Taiwan ||
        s.language == LanguageType::SimplifiedChinese || s.language == LanguageType::Korean ||
        s.language == LanguageType::TraditionalChinese) {
        s.region = RegionType::USA;
        s.language = LanguageType::English;
    }

    FSInit();
    s.fs_initialized = true;

    s.fs_client = static_cast<FSClient *>(SDL_aligned_alloc(0x40, sizeof(FSClient)));
    if (!s.fs_client) {
        ReleaseCreateResources();
        return SDL_OutOfMemory();
    }
    SDL_memset(s.fs_client, 0, sizeof(FSClient));
    if (FSAddClient(s.fs_client, FS_ERROR_FLAG_ALL) != FS_STATUS_OK) {
        ReleaseCreateResources();
        return SDL_SetError("FSAddClient() failed for the software keyboard");
    }
    s.fs_client_added = true;

    work_size = nn::swkbd::GetWorkMemorySize(0);
    s.work_memory = SDL_aligned_alloc(0x40, work_size);
    if (!s.work_memory) {
        ReleaseCreateResources();
        return SDL_OutOfMemory();
    }

    arg.workMemory = s.work_memory;
    arg.regionType = s.region;
    arg.fsClient = s.fs_client;
    if (!nn::swkbd::Create(arg)) {
        ReleaseCreateResources();
        return SDL_SetError("nn::swkbd::Create() failed");
    }
    s.created = true;
    return true;
}

} // namespace

bool WIIU_SWKBD_Show(const WIIU_SWKBDOptions *options)
{
    nn::swkbd::AppearArg arg; // the constructors set the defaults

    if (!Create()) {
        return false;
    }

    FreeStrings();
    s.initial_text = ToUTF16(options->initial_text);
    s.hint_text = ToUTF16(options->hint_text);
    s.ok_label = ToUTF16(options->ok_label);

    arg.keyboardArg.configArg.languageType = s.language;
    arg.keyboardArg.configArg.keyboardLayout = DefaultLayout(s.region, s.language); // also selects the dictionary
    arg.keyboardArg.configArg.keyboardMode = options->numpad ? nn::swkbd::KeyboardMode::Numpad : nn::swkbd::KeyboardMode::Full;
    arg.keyboardArg.configArg.controllerType = nn::swkbd::ControllerType::DrcGamepad;
    arg.keyboardArg.configArg.showWordSuggestions = options->word_suggestions;
    arg.keyboardArg.configArg.disableNewLine = options->disable_newline;
    arg.keyboardArg.configArg.drawSysWiiPointer = false; // no Wii Remote input
    if (s.ok_label) {
        arg.keyboardArg.configArg.okString = s.ok_label;
    }
    arg.inputFormArg.initialText = s.initial_text;
    arg.inputFormArg.hintText = s.hint_text;
    arg.inputFormArg.maxTextLength = options->max_length;
    arg.inputFormArg.passwordMode = options->password_hidden ? nn::swkbd::PasswordMode::Hide : nn::swkbd::PasswordMode::Clear;

    if (!nn::swkbd::AppearInputForm(arg)) {
        return SDL_SetError("nn::swkbd::AppearInputForm() failed");
    }
    s.shown = true;
    s.closing = false;
    s.frames_hidden = 0;
    return true;
}

void WIIU_SWKBD_Hide(void)
{
    if (!s.created || !s.shown) {
        return;
    }
    nn::swkbd::DisappearInputForm();
    s.closing = true;
}

bool WIIU_SWKBD_IsActive(void)
{
    // Not the nn::swkbd state: it may read Hidden right after Show(), see kMaxHiddenFrames
    return s.created && s.shown;
}

WIIU_SWKBDResult WIIU_SWKBD_Update(VPADStatus *vpad, char **text)
{
    WIIU_SWKBDResult result = WIIU_SWKBD_RESULT_NONE;
    nn::swkbd::ControllerInfo info;

    *text = nullptr;
    if (!s.created || !s.shown) {
        return result;
    }

    info.vpad = vpad;
    nn::swkbd::Calc(info);
    if (nn::swkbd::IsNeedCalcSubThreadFont()) {
        nn::swkbd::CalcSubThreadFont();
    }
    if (nn::swkbd::IsNeedCalcSubThreadPredict()) {
        nn::swkbd::CalcSubThreadPredict();
    }

    if (!s.closing) {
        if (nn::swkbd::IsDecideOkButton(nullptr)) {
            *text = ToUTF8(nn::swkbd::GetInputFormString());
            result = WIIU_SWKBD_RESULT_OK;
        } else if (nn::swkbd::IsDecideCancelButton(nullptr)) {
            result = WIIU_SWKBD_RESULT_CANCEL;
        }
        if (result != WIIU_SWKBD_RESULT_NONE) {
            // Either way the input form goes away
            nn::swkbd::DisappearInputForm();
            s.closing = true;
        }
    }

    if (nn::swkbd::GetStateInputForm() == nn::swkbd::State::Hidden) {
        if (s.closing) {
            s.shown = false;
            s.closing = false;
            FreeStrings();
        } else if (++s.frames_hidden >= kMaxHiddenFrames) {
            // It never appeared (or went away by itself): treat it as cancelled
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "The software keyboard did not appear");
            s.shown = false;
            FreeStrings();
            result = WIIU_SWKBD_RESULT_CANCEL;
        }
    } else {
        s.frames_hidden = 0;
    }
    return result;
}

void WIIU_SWKBD_Draw(int output)
{
    if (!s.created || !s.shown || nn::swkbd::GetStateInputForm() == nn::swkbd::State::Hidden) {
        return;
    }
    if (output == 0) {
        nn::swkbd::DrawTV();
    } else {
        nn::swkbd::DrawDRC();
    }
}

void WIIU_SWKBD_Shutdown(void)
{
    if (s.created) {
        nn::swkbd::Destroy();
        s.created = false;
    }
    s.shown = false;
    s.closing = false;
    FreeStrings();
    ReleaseCreateResources();
}

#endif // SDL_VIDEO_DRIVER_WIIU
