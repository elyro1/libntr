#include <SDL2/SDL_keycode.h>
#include <nitro.h>
#include <simulator/sim.h>
#include <simulator/config/config_check.h>
#include <simulator/config/ini.h>
#include <simulator/config/sim_config.h>

#include <string.h>
#include <wchar.h>

#include <SDL2/SDL.h>

#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>

#define SIM_CONFIG_FILE_PATH "./sim_config.ini"

static int IniHandler(void* user, const char* section, const char* name,
                   const char* value, int line);
static const char * BoolToString(BOOL arg);
static BOOL ParseBool(const char *value, BOOL *out);
static BOOL ParseInt(const char *value, int *out);
static void WriteConfig(SIM_ConfigText *text, const SIM_config_type *aConfig);

static SIM_ConfigCheck sCheck;
// The key being handled, for warnings about its value
static const char *sSection, *sName;
static int sLine;

// Keys the writer no longer emits but that are still read
static const SIM_ConfigKeyName sLegacyKeys[] = {
    { "Net", "ServerIp" },
};

// Invalid values keep the setting's previous value, with a warning
#define SET_BOOL(field) do { BOOL parsed_; if (ParseBool(value, &parsed_)) (field) = parsed_; } while (0)
#define SET_INT(field) do { int parsed_; if (ParseInt(value, &parsed_)) (field) = parsed_; } while (0)
static const char * ScreenLayoutTypeToString(SIM_config_screen_layout_type layout);
static SIM_config_screen_layout_type StringToScreenLayoutType(const char * str);


static int IniHandler(void* user, const char* section, const char* name,
                   const char* value, int line)
{
    #define MATCH(s, n) (strcmp(section, s) == 0 && strcmp(name, n) == 0)

    SIM_config_type * config = (SIM_config_type *)user;

    if (!SIM_ConfigCheck_Key(&sCheck, section, name, line)) {
        return 1;
    }
    sSection = section;
    sName = name;
    sLine = line;

    if(MATCH("Interface", "MenuScalePercent")) {
        char *end;
        errno = 0;
        long scale = strtol(value, &end, 10);
        BOOL parsed = end != value;
        while (isspace((unsigned char)*end)) ++end;
        config->menuScalePercent = (!parsed || *end || errno || scale < 100 || scale > 200 || scale % 25)
            ? 0 : (int)scale;
    }
    if(MATCH("Interface", "PauseWhileOpen")) {
        SET_BOOL(config->pauseWhileMenuOpen);
    }

    // General section
    if(MATCH("General", "InternalResolutionScale")) {
        int scale = config->internalResolutionScale;
        ParseInt(value, &scale);
        config->internalResolutionScale = scale < 1 ? 1 : (scale > 20 ? 20 : scale);
    }
    if(MATCH("General", "MasterVolume")) {
        int volume = config->masterVolume;
        ParseInt(value, &volume);
        config->masterVolume = volume < 0 ? 0 : (volume > 127 ? 127 : volume);
    }
    if(MATCH("General", "WindowHeight")) {
        SET_INT(config->windowHeight);
    }
    if(MATCH("General", "WindowWidth")) {
        SET_INT(config->windowWidth);
    }
    if(MATCH("General", "FullScreen")) {
        SET_BOOL(config->fullScreen);
    }
    if(MATCH("General", "ScreenLayout")) {
        if (strcmp(value, ScreenLayoutTypeToString(StringToScreenLayoutType(value))) == 0) {
            config->screenLayout = StringToScreenLayoutType(value);
        } else {
            SIM_ConfigCheck_BadValue(&sCheck, section, name, value, line, "vertical, horizontal, top_only or large");
        }
    }
    if(MATCH("General", "SwapScreens")) {
        SET_BOOL(config->swapScreens);
    }
    if(MATCH("General", "VsyncInterval")) {
        SET_INT(config->vsyncInterval);
    }
    if(MATCH("General", "CapFrameRate")) {
        SET_BOOL(config->capFrameRate);
    }
    if(MATCH("General", "FastForwardMultiplier")) {
        char *end;
        errno = 0;
        float multiplier = strtof(value, &end);
        BOOL parsed = end != value;
        while (isspace((unsigned char)*end)) ++end;
        config->fastForwardMultiplier = (!parsed || *end || errno)
            ? 5.0f : SIM_Config_ValidateFastForwardMultiplier(multiplier);
    }

    if(MATCH("General", "MacAddr")) {
        const char * pos = value;
        if(strlen(value) >= 12) {
            for (size_t count = 0; count < 6; count++) {
                sscanf(pos, "%2hhx", &config->macAddr[count]);
                pos += 2;
            }
        }
    }

    // PAD
    if(MATCH("PAD", "JoyAxisDeadzone")) {
        char *end;
        errno = 0;
        long deadzone = strtol(value, &end, 10);
        BOOL parsed = end != value;
        while (isspace((unsigned char)*end)) ++end;
        config->padSettings.joyAxisDeadzone = (!parsed || *end || errno)
            ? SIM_CONFIG_JOY_DEFAULT_DEADZONE
            : deadzone < 0 ? 0
            : deadzone > SIM_CONFIG_JOY_MAX_DEADZONE ? SIM_CONFIG_JOY_MAX_DEADZONE
            : (int)deadzone;
    }
    if(MATCH("PAD", "AKey")) {
        SET_INT(config->padSettings.aKey);
    }
    if(MATCH("PAD", "BKey")) {
        SET_INT(config->padSettings.bKey);
    }
    if(MATCH("PAD", "XKey")) {
        SET_INT(config->padSettings.xKey);
    }
    if(MATCH("PAD", "YKey")) {
        SET_INT(config->padSettings.yKey);
    }
    if(MATCH("PAD", "UpKey")) {
        SET_INT(config->padSettings.upKey);
    }
    if(MATCH("PAD", "DownKey")) {
        SET_INT(config->padSettings.downKey);
    }
    if(MATCH("PAD", "LeftKey")) {
        SET_INT(config->padSettings.leftKey);
    }
    if(MATCH("PAD", "RightKey")) {
        SET_INT(config->padSettings.rightKey);
    }
    if(MATCH("PAD", "LKey")) {
        SET_INT(config->padSettings.lKey);
    }
    if(MATCH("PAD", "RKey")) {
        SET_INT(config->padSettings.rKey);
    }
    if(MATCH("PAD", "StartKey")) {
        SET_INT(config->padSettings.startKey);
    }
    if(MATCH("PAD", "SelectKey")) {
        SET_INT(config->padSettings.selectKey);
    }
    if(MATCH("PAD", "FastForwardKey")) {
        SET_INT(config->padSettings.fastForwardKey);
    }
    if(MATCH("PAD", "FullScreenKey")) {
        SET_INT(config->padSettings.fullScreenKey);
    }
    if(MATCH("PAD", "GUIKey")) {
        SET_INT(config->padSettings.guiKey);
    }
    if(MATCH("PAD", "AJoyKey")) {
        SET_INT(config->padSettings.aJoyKey);
    }
    if(MATCH("PAD", "BJoyKey")) {
        SET_INT(config->padSettings.bJoyKey);
    }
    if(MATCH("PAD", "XJoyKey")) {
        SET_INT(config->padSettings.xJoyKey);
    }
    if(MATCH("PAD", "YJoyKey")) {
        SET_INT(config->padSettings.yJoyKey);
    }
    if(MATCH("PAD", "UpJoyKeySecondary")) {
        SET_INT(config->padSettings.upJoyKeySecondary);
    }
    if(MATCH("PAD", "UpJoyKey")) {
        SET_INT(config->padSettings.upJoyKey);
    }
    if(MATCH("PAD", "DownJoyKeySecondary")) {
        SET_INT(config->padSettings.downJoyKeySecondary);
    }
    if(MATCH("PAD", "DownJoyKey")) {
        SET_INT(config->padSettings.downJoyKey);
    }
    if(MATCH("PAD", "LeftJoyKeySecondary")) {
        SET_INT(config->padSettings.leftJoyKeySecondary);
    }
    if(MATCH("PAD", "LeftJoyKey")) {
        SET_INT(config->padSettings.leftJoyKey);
    }
    if(MATCH("PAD", "RightJoyKeySecondary")) {
        SET_INT(config->padSettings.rightJoyKeySecondary);
    }
    if(MATCH("PAD", "RightJoyKey")) {
        SET_INT(config->padSettings.rightJoyKey);
    }
    if(MATCH("PAD", "LJoyKey")) {
        SET_INT(config->padSettings.lJoyKey);
    }
    if(MATCH("PAD", "RJoyKey")) {
        SET_INT(config->padSettings.rJoyKey);
    }
    if(MATCH("PAD", "StartJoyKey")) {
        SET_INT(config->padSettings.startJoyKey);
    }
    if(MATCH("PAD", "SelectJoyKey")) {
        SET_INT(config->padSettings.selectJoyKey);
    }

    //OS NVRAM
    //Owner Info
    if(MATCH("OSNVRAM", "Owner_FavoriteColor")) {
        SET_INT(config->osNvramConfig.ncd.owner.favoriteColor);
    }
    if(MATCH("OSNVRAM", "Owner_BirthdayMonth")) {
        SET_INT(config->osNvramConfig.ncd.owner.birthday.month);
    }
    if(MATCH("OSNVRAM", "Owner_BirthdayDay")) {
        SET_INT(config->osNvramConfig.ncd.owner.birthday.day);
    }
    if(MATCH("OSNVRAM", "Owner_Nickname")) {
        memset(config->osNvramConfig.ncd.owner.nickname.str, 0, NVRAM_CONFIG_NICKNAME_LENGTH*2);

        int valueLen = strlen(value);
        for(int i=0; i < valueLen; i++) {
            u16 outChar = value[i];
            config->osNvramConfig.ncd.owner.nickname.str[i] = outChar;
        }
        config->osNvramConfig.ncd.owner.nickname.length = (u8)wcslen((wchar_t*)config->osNvramConfig.ncd.owner.nickname.str);
    }
    if(MATCH("OSNVRAM", "Owner_Comment")) {
        memset(config->osNvramConfig.ncd.owner.comment.str, 0, NVRAM_CONFIG_COMMENT_LENGTH*2);
        swprintf((wchar_t*)config->osNvramConfig.ncd.owner.comment.str, (NVRAM_CONFIG_COMMENT_LENGTH)-1, L"%s", value);
        config->osNvramConfig.ncd.owner.comment.length = wcslen((wchar_t*)config->osNvramConfig.ncd.owner.comment.str);
    }
    //Alarm
    if(MATCH("OSNVRAM", "Alarm_Hour")) {
        SET_INT(config->osNvramConfig.ncd.alarm.hour);
    }
    if(MATCH("OSNVRAM", "Alarm_Minute")) {
        SET_INT(config->osNvramConfig.ncd.alarm.minute);
    }
    if(MATCH("OSNVRAM", "Alarm_Second")) {
        SET_INT(config->osNvramConfig.ncd.alarm.second);
    }
    if(MATCH("OSNVRAM", "Alarm_DaysOfWeek")) {
        SET_INT(config->osNvramConfig.ncd.alarm.enableWeek);
    }
    if(MATCH("OSNVRAM", "Alarm_Enabled")) {
        SET_BOOL(config->osNvramConfig.ncd.alarm.alarmOn);
    }
    //Touch panel calibration
    if(MATCH("OSNVRAM", "TP_RawX1")) {
        SET_INT(config->osNvramConfig.ncd.tp.raw_x1);
    }
    if(MATCH("OSNVRAM", "TP_RawY1")) {
        SET_INT(config->osNvramConfig.ncd.tp.raw_y1);
    }
    if(MATCH("OSNVRAM", "TP_dx1")) {
        SET_INT(config->osNvramConfig.ncd.tp.dx1);
    }
    if(MATCH("OSNVRAM", "TP_dy1")) {
        SET_INT(config->osNvramConfig.ncd.tp.dy1);
    }
    if(MATCH("OSNVRAM", "TP_RawX2")) {
        SET_INT(config->osNvramConfig.ncd.tp.raw_x2);
    }
    if(MATCH("OSNVRAM", "TP_RawY2")) {
        SET_INT(config->osNvramConfig.ncd.tp.raw_y2);
    }
    if(MATCH("OSNVRAM", "TP_dx2")) {
        SET_INT(config->osNvramConfig.ncd.tp.dx2);
    }
    if(MATCH("OSNVRAM", "TP_dy2")) {
        SET_INT(config->osNvramConfig.ncd.tp.dy2);
    }
    //Options
    if(MATCH("OSNVRAM", "Option_Language")) {
        SET_INT(config->osNvramConfig.ncd.option.language);
    }
    if(MATCH("OSNVRAM", "Option_AGBLCD")) {
        SET_INT(config->osNvramConfig.ncd.option.agbLcd);
    }
    if(MATCH("OSNVRAM", "Option_PullOutCardFlag")) {
        SET_INT(config->osNvramConfig.ncd.option.detectPullOutCardFlag);
    }
    if(MATCH("OSNVRAM", "Option_PullOutCtrdgFlag")) {
        SET_INT(config->osNvramConfig.ncd.option.detectPullOutCtrdgFlag);
    }
    if(MATCH("OSNVRAM", "Option_AutoBootFlag")) {
        SET_BOOL(config->osNvramConfig.ncd.option.autoBootFlag);
    }
    if(MATCH("OSNVRAM", "Option_FavoriteColorSet")) {
        SET_BOOL(config->osNvramConfig.ncd.option.input_favoriteColor);
    }
    if(MATCH("OSNVRAM", "Option_TouchPanelCalibrated")) {
        SET_BOOL(config->osNvramConfig.ncd.option.input_tp);
    }
    if(MATCH("OSNVRAM", "Option_LanguageSet")) {
        SET_BOOL(config->osNvramConfig.ncd.option.input_language);
    }
    if(MATCH("OSNVRAM", "Option_RTCSet")) {
        SET_BOOL(config->osNvramConfig.ncd.option.input_rtc);
    }
    if(MATCH("OSNVRAM", "Option_NicknameSet")) {
        SET_BOOL(config->osNvramConfig.ncd.option.input_nickname);
    }
    if(MATCH("OSNVRAM", "Option_Timezone")) {
        SET_INT(config->osNvramConfig.ncd.option.timezone);
    }
    if(MATCH("OSNVRAM", "Option_RTCClockAdjust")) {
        SET_INT(config->osNvramConfig.ncd.option.rtcClockAdjust);
    }
    if(MATCH("OSNVRAM", "Option_RTCOffset")) {
        SET_INT(config->osNvramConfig.ncd.option.rtcOffset);
    }

    // Net Settings
    if(MATCH("LAN", "ServerIp") || MATCH("Net", "ServerIp")) {
        memset(config->netSettings.serverIp, 0, sizeof(config->netSettings.serverIp));
        strncpy(config->netSettings.serverIp, value, sizeof(config->netSettings.serverIp)-1);
        if (strcmp(section, "Net") == 0) {
            SIM_ConfigCheck_SetSource(&sCheck, "LAN", "ServerIp", line, "from [Net]");
        }
    }
    return 1;
}

static const char * BoolToString(BOOL arg)
{
    const char * ret = "false";
    if(arg) {
        ret = "true";
    }

    return ret;
}

static BOOL ParseBool(const char *value, BOOL *out)
{
    if (strcmp(value, "true") == 0 || strcmp(value, "false") == 0) {
        *out = value[0] == 't';
        return TRUE;
    }
    SIM_ConfigCheck_BadValue(&sCheck, sSection, sName, value, sLine, "true or false");
    return FALSE;
}

static BOOL ParseInt(const char *value, int *out)
{
    char *end;
    errno = 0;
    long number = strtol(value, &end, 10);
    while (isspace((unsigned char)*end)) ++end;
    if (end == value || *end || errno || number < INT_MIN || number > INT_MAX) {
        SIM_ConfigCheck_BadValue(&sCheck, sSection, sName, value, sLine, "a whole number");
        return FALSE;
    }
    *out = (int)number;
    return TRUE;
}

static const char * ScreenLayoutTypeToString(SIM_config_screen_layout_type layout)
{
    const char * ret = "";

    switch(layout) {
        default:
        case SIM_CONFIG_SCREEN_LAYOUT_VERTICAL:
            ret = "vertical";
            break;
        case SIM_CONFIG_SCREEN_LAYOUT_HORIZONTAL:
            ret = "horizontal";
            break;
        case SIM_CONFIG_SCREEN_LAYOUT_TOP_ONLY:
            ret = "top_only";
            break;
        case SIM_CONFIG_SCREEN_LAYOUT_LARGE:
            ret = "large";
            break;
    }
    return ret;
}

static SIM_config_screen_layout_type StringToScreenLayoutType(const char * str)
{
    SIM_config_screen_layout_type ret = SIM_CONFIG_SCREEN_LAYOUT_VERTICAL;

    if(strcmp(str, "vertical") == 0) {
        ret = SIM_CONFIG_SCREEN_LAYOUT_VERTICAL;
    } else if (strcmp(str, "horizontal") == 0) {
        ret = SIM_CONFIG_SCREEN_LAYOUT_HORIZONTAL;
    } else if (strcmp(str, "top_only") == 0) {
        ret = SIM_CONFIG_SCREEN_LAYOUT_TOP_ONLY;
    } else if (strcmp(str, "large") == 0) {
        ret = SIM_CONFIG_SCREEN_LAYOUT_LARGE;
    }
    
    return ret;
}



// Loads the configuration file. If it does not exist, return false
BOOL SIM_Config_LoadConfigFile(SIM_config_type * aConfig)
{
    SIM_ConfigText text = { 0 };
    WriteConfig(&text, aConfig);
    SIM_ConfigCheck_Init(&sCheck, SIM_CONFIG_FILE_PATH, SIM_ConfigText_Get(&text), sLegacyKeys, sizeof(sLegacyKeys) / sizeof(sLegacyKeys[0]));
    SIM_ConfigText_Free(&text);

    int result = ini_parse(SIM_CONFIG_FILE_PATH, IniHandler, aConfig);
    SIM_ConfigCheck_ParseResult(&sCheck, result, "it is ignored (later such lines are not reported)");
    return result >= 0;
}

void SIM_Config_PrintEffective(const SIM_config_type *aConfig, FILE *out)
{
    SIM_ConfigText text = { 0 };
    WriteConfig(&text, aConfig);
    fprintf(out, "; %s\n", SIM_CONFIG_FILE_PATH + 2);
    SIM_ConfigCheck_Print(&sCheck, SIM_ConfigText_Get(&text), out);
    SIM_ConfigText_Free(&text);
}

// Initialize a SIM_config_type to default values
float SIM_Config_ValidateFastForwardMultiplier(float multiplier)
{
    if (!isfinite(multiplier)) return 5.0f;
    if (multiplier < 1.0f) return 1.0f;
    if (multiplier > 10.0f) return 10.0f;
    return multiplier;
}

void SIM_Config_LoadDefaults(SIM_config_type * aConfig)
{
#ifdef SDK_BUILD_NX
    aConfig->internalResolutionScale = 2;
    aConfig->windowHeight = 1080;
    aConfig->windowWidth = 1920;
#else
    aConfig->internalResolutionScale = 10;
    aConfig->windowHeight = 0;
    aConfig->windowWidth = 0;
#endif
    aConfig->fullScreen = FALSE;
    aConfig->screenLayout = SIM_CONFIG_SCREEN_LAYOUT_TOP_ONLY;
    aConfig->swapScreens = FALSE;
    aConfig->vsyncInterval = 1;
    aConfig->capFrameRate = TRUE;
    aConfig->masterVolume = 50;
    aConfig->fastForwardMultiplier = 5.0f;
    aConfig->menuScalePercent = 0;
    aConfig->pauseWhileMenuOpen = FALSE;

    aConfig->padSettings.joyAxisDeadzone = SIM_CONFIG_JOY_DEFAULT_DEADZONE;
    aConfig->padSettings.upJoyKeySecondary = SIM_CONFIG_JOY_AXIS_MINUS_MASK | 1;
    aConfig->padSettings.downJoyKeySecondary = SIM_CONFIG_JOY_AXIS_PLUS_MASK | 1;
    aConfig->padSettings.leftJoyKeySecondary = SIM_CONFIG_JOY_AXIS_MINUS_MASK;
    aConfig->padSettings.rightJoyKeySecondary = SIM_CONFIG_JOY_AXIS_PLUS_MASK;
    aConfig->padSettings.aKey = SDLK_z;
    aConfig->padSettings.bKey = SDLK_x;
    aConfig->padSettings.xKey = SDLK_c;
    aConfig->padSettings.yKey = SDLK_v;
    aConfig->padSettings.upKey = SDLK_UP;
    aConfig->padSettings.downKey = SDLK_DOWN;
    aConfig->padSettings.leftKey = SDLK_LEFT;
    aConfig->padSettings.rightKey = SDLK_RIGHT;
    aConfig->padSettings.lKey = SDLK_q;
    aConfig->padSettings.rKey = SDLK_e;
    aConfig->padSettings.startKey = SDLK_RETURN;
    aConfig->padSettings.selectKey = SDLK_RSHIFT;
    aConfig->padSettings.guiKey = SDLK_TAB;
    aConfig->padSettings.fastForwardKey = SDLK_LSHIFT;
    aConfig->padSettings.fullScreenKey = SDLK_F11;
    aConfig->padSettings.aJoyKey = 0;
    aConfig->padSettings.bJoyKey = 1;
    aConfig->padSettings.xJoyKey = 2;
    aConfig->padSettings.yJoyKey = 3;
#ifdef SDK_BUILD_NX
    aConfig->padSettings.upJoyKey = 13;
    aConfig->padSettings.downJoyKey = 15;
    aConfig->padSettings.leftJoyKey = 12;
    aConfig->padSettings.rightJoyKey = 14;
    aConfig->padSettings.lJoyKey = 6;
    aConfig->padSettings.rJoyKey = 7;
    aConfig->padSettings.startJoyKey = 10;
    aConfig->padSettings.selectJoyKey = 11;
#else
    aConfig->padSettings.upJoyKey = SDL_HAT_UP | SIM_CONFIG_JOY_HAT_MASK;
    aConfig->padSettings.downJoyKey = SDL_HAT_DOWN | SIM_CONFIG_JOY_HAT_MASK;
    aConfig->padSettings.leftJoyKey = SDL_HAT_LEFT | SIM_CONFIG_JOY_HAT_MASK;
    aConfig->padSettings.rightJoyKey = SDL_HAT_RIGHT | SIM_CONFIG_JOY_HAT_MASK;
    aConfig->padSettings.lJoyKey = 9;
    aConfig->padSettings.rJoyKey = 10;
    aConfig->padSettings.startJoyKey = 6;
    aConfig->padSettings.selectJoyKey = 4;
#endif

    // Set a random MAC address (with a nintendo prefix)
    aConfig->macAddr[0] = 0x98;
    aConfig->macAddr[1] = 0xE2;
    aConfig->macAddr[2] = 0x55;

    srand(time(NULL));
    aConfig->macAddr[3] = rand();
    aConfig->macAddr[4] = rand();
    aConfig->macAddr[5] = rand();

    memset(&aConfig->osNvramConfig, 0, sizeof(NVRAMConfig));
    wcscpy((wchar_t*)aConfig->osNvramConfig.ncd.owner.nickname.str, L"Player");
    aConfig->osNvramConfig.ncd.option.input_nickname = TRUE;
    aConfig->osNvramConfig.ncd.option.input_favoriteColor = TRUE;
    aConfig->osNvramConfig.ncd.option.input_language = TRUE;
    aConfig->osNvramConfig.ncd.option.input_tp = TRUE;
    aConfig->osNvramConfig.ncd.option.input_rtc = TRUE;
    
    aConfig->netSettings.serverIp[15] = 0;
    strcpy(aConfig->netSettings.serverIp, "127.0.0.1");
}

// Writes every key, also to tell SIM_ConfigCheck which keys the file has
static void WriteConfig(SIM_ConfigText *configFile, const SIM_config_type *aConfig)
{
    SIM_ConfigText_Printf(configFile, "[Interface]\n");
    SIM_ConfigText_Printf(configFile, "MenuScalePercent=%d\n", aConfig->menuScalePercent);
    SIM_ConfigText_Printf(configFile, "PauseWhileOpen=%s\n\n", BoolToString(aConfig->pauseWhileMenuOpen));
    
    SIM_ConfigText_Printf(configFile, "[General]\n");
    SIM_ConfigText_Printf(configFile, "InternalResolutionScale=%d\n", aConfig->internalResolutionScale);
    SIM_ConfigText_Printf(configFile, "MasterVolume=%d\n", aConfig->masterVolume);
    SIM_ConfigText_Printf(configFile, "WindowHeight=%d\n", aConfig->windowHeight);
    SIM_ConfigText_Printf(configFile, "WindowWidth=%d\n", aConfig->windowWidth);
    SIM_ConfigText_Printf(configFile, "FullScreen=%s\n", BoolToString(aConfig->fullScreen));

    const char * screenLayoutString = ScreenLayoutTypeToString(aConfig->screenLayout);
    SIM_ConfigText_Printf(configFile, "ScreenLayout=%s\n", screenLayoutString);
    SIM_ConfigText_Printf(configFile, "SwapScreens=%s\n", BoolToString(aConfig->swapScreens));
    SIM_ConfigText_Printf(configFile, "VsyncInterval=%d\n", aConfig->vsyncInterval);
    SIM_ConfigText_Printf(configFile, "CapFrameRate=%s\n", BoolToString(aConfig->capFrameRate));
    SIM_ConfigText_Printf(configFile, "FastForwardMultiplier=%.6g\n",
        SIM_Config_ValidateFastForwardMultiplier(aConfig->fastForwardMultiplier));
    SIM_ConfigText_Printf(configFile, "MacAddr=%02x%02x%02x%02x%02x%02x\n", aConfig->macAddr[0],
                                                              aConfig->macAddr[1],
                                                              aConfig->macAddr[2],
                                                              aConfig->macAddr[3],
                                                              aConfig->macAddr[4],
                                                              aConfig->macAddr[5]);
    SIM_ConfigText_Printf(configFile, "\n");

    SIM_ConfigText_Printf(configFile, "[PAD]\n");
    SIM_ConfigText_Printf(configFile, "JoyAxisDeadzone=%d\n", aConfig->padSettings.joyAxisDeadzone);
    SIM_ConfigText_Printf(configFile, "AKey=%d\n", aConfig->padSettings.aKey);
    SIM_ConfigText_Printf(configFile, "BKey=%d\n", aConfig->padSettings.bKey);
    SIM_ConfigText_Printf(configFile, "XKey=%d\n", aConfig->padSettings.xKey);
    SIM_ConfigText_Printf(configFile, "YKey=%d\n", aConfig->padSettings.yKey);
    SIM_ConfigText_Printf(configFile, "UpKey=%d\n", aConfig->padSettings.upKey);
    SIM_ConfigText_Printf(configFile, "DownKey=%d\n", aConfig->padSettings.downKey);
    SIM_ConfigText_Printf(configFile, "LeftKey=%d\n", aConfig->padSettings.leftKey);
    SIM_ConfigText_Printf(configFile, "RightKey=%d\n", aConfig->padSettings.rightKey);
    SIM_ConfigText_Printf(configFile, "LKey=%d\n", aConfig->padSettings.lKey);
    SIM_ConfigText_Printf(configFile, "RKey=%d\n", aConfig->padSettings.rKey);
    SIM_ConfigText_Printf(configFile, "StartKey=%d\n", aConfig->padSettings.startKey);
    SIM_ConfigText_Printf(configFile, "SelectKey=%d\n", aConfig->padSettings.selectKey);
    SIM_ConfigText_Printf(configFile, "GUIKey=%d\n", aConfig->padSettings.guiKey);
    SIM_ConfigText_Printf(configFile, "FastForwardKey=%d\n", aConfig->padSettings.fastForwardKey);
    SIM_ConfigText_Printf(configFile, "FullScreenKey=%d\n", aConfig->padSettings.fullScreenKey);
    SIM_ConfigText_Printf(configFile, "AJoyKey=%d\n", aConfig->padSettings.aJoyKey);
    SIM_ConfigText_Printf(configFile, "BJoyKey=%d\n", aConfig->padSettings.bJoyKey);
    SIM_ConfigText_Printf(configFile, "XJoyKey=%d\n", aConfig->padSettings.xJoyKey);
    SIM_ConfigText_Printf(configFile, "YJoyKey=%d\n", aConfig->padSettings.yJoyKey);
    SIM_ConfigText_Printf(configFile, "UpJoyKey=%d\n", aConfig->padSettings.upJoyKey);
    SIM_ConfigText_Printf(configFile, "UpJoyKeySecondary=%d\n", aConfig->padSettings.upJoyKeySecondary);
    SIM_ConfigText_Printf(configFile, "DownJoyKey=%d\n", aConfig->padSettings.downJoyKey);
    SIM_ConfigText_Printf(configFile, "DownJoyKeySecondary=%d\n", aConfig->padSettings.downJoyKeySecondary);
    SIM_ConfigText_Printf(configFile, "LeftJoyKey=%d\n", aConfig->padSettings.leftJoyKey);
    SIM_ConfigText_Printf(configFile, "LeftJoyKeySecondary=%d\n", aConfig->padSettings.leftJoyKeySecondary);
    SIM_ConfigText_Printf(configFile, "RightJoyKey=%d\n", aConfig->padSettings.rightJoyKey);
    SIM_ConfigText_Printf(configFile, "RightJoyKeySecondary=%d\n", aConfig->padSettings.rightJoyKeySecondary);
    SIM_ConfigText_Printf(configFile, "LJoyKey=%d\n", aConfig->padSettings.lJoyKey);
    SIM_ConfigText_Printf(configFile, "RJoyKey=%d\n", aConfig->padSettings.rJoyKey);
    SIM_ConfigText_Printf(configFile, "StartJoyKey=%d\n", aConfig->padSettings.startJoyKey);
    SIM_ConfigText_Printf(configFile, "SelectJoyKey=%d\n", aConfig->padSettings.selectJoyKey);
    SIM_ConfigText_Printf(configFile, "\n");

    SIM_ConfigText_Printf(configFile, "[OSNVRAM]\n");
    SIM_ConfigText_Printf(configFile, "Owner_FavoriteColor=%d\n", aConfig->osNvramConfig.ncd.owner.favoriteColor);
    SIM_ConfigText_Printf(configFile, "Owner_BirthdayMonth=%d\n", aConfig->osNvramConfig.ncd.owner.birthday.month);
    SIM_ConfigText_Printf(configFile, "Owner_BirthdayDay=%d\n", aConfig->osNvramConfig.ncd.owner.birthday.day);
    SIM_ConfigText_Printf(configFile, "Owner_Nickname=%ls\n", aConfig->osNvramConfig.ncd.owner.nickname.str);
    SIM_ConfigText_Printf(configFile, "Owner_Comment=%ls\n", aConfig->osNvramConfig.ncd.owner.comment.str);
    SIM_ConfigText_Printf(configFile, "Alarm_Hour=%d\n", aConfig->osNvramConfig.ncd.alarm.hour);
    SIM_ConfigText_Printf(configFile, "Alarm_Minute=%d\n", aConfig->osNvramConfig.ncd.alarm.minute);
    SIM_ConfigText_Printf(configFile, "Alarm_Second=%d\n", aConfig->osNvramConfig.ncd.alarm.second);
    SIM_ConfigText_Printf(configFile, "Alarm_DaysOfWeek=%d\n", aConfig->osNvramConfig.ncd.alarm.enableWeek);
    SIM_ConfigText_Printf(configFile, "Alarm_Enabled=%s\n", BoolToString(aConfig->osNvramConfig.ncd.alarm.alarmOn));
    SIM_ConfigText_Printf(configFile, "TP_RawX1=%d\n", aConfig->osNvramConfig.ncd.tp.raw_x1);
    SIM_ConfigText_Printf(configFile, "TP_RawY1=%d\n", aConfig->osNvramConfig.ncd.tp.raw_y1);
    SIM_ConfigText_Printf(configFile, "TP_dx1=%d\n", aConfig->osNvramConfig.ncd.tp.dx1);
    SIM_ConfigText_Printf(configFile, "TP_dy1=%d\n", aConfig->osNvramConfig.ncd.tp.dy1);
    SIM_ConfigText_Printf(configFile, "TP_RawX2=%d\n", aConfig->osNvramConfig.ncd.tp.raw_x2);
    SIM_ConfigText_Printf(configFile, "TP_RawY2=%d\n", aConfig->osNvramConfig.ncd.tp.raw_y2);
    SIM_ConfigText_Printf(configFile, "TP_dx2=%d\n", aConfig->osNvramConfig.ncd.tp.dx2);
    SIM_ConfigText_Printf(configFile, "TP_dy2=%d\n", aConfig->osNvramConfig.ncd.tp.dy2);
    SIM_ConfigText_Printf(configFile, "Option_Language=%d\n", aConfig->osNvramConfig.ncd.option.language);
    SIM_ConfigText_Printf(configFile, "Option_AGBLCD=%d\n", aConfig->osNvramConfig.ncd.option.agbLcd);
    SIM_ConfigText_Printf(configFile, "Option_PullOutCardFlag=%d\n", aConfig->osNvramConfig.ncd.option.detectPullOutCardFlag);
    SIM_ConfigText_Printf(configFile, "Option_PullOutCtrdgFlag=%d\n", aConfig->osNvramConfig.ncd.option.detectPullOutCtrdgFlag);
    SIM_ConfigText_Printf(configFile, "Option_AutoBootFlag=%s\n", BoolToString(aConfig->osNvramConfig.ncd.option.autoBootFlag));
    SIM_ConfigText_Printf(configFile, "Option_FavoriteColorSet=%s\n", BoolToString(aConfig->osNvramConfig.ncd.option.input_favoriteColor));
    SIM_ConfigText_Printf(configFile, "Option_TouchPanelCalibrated=%s\n", BoolToString(aConfig->osNvramConfig.ncd.option.input_tp));
    SIM_ConfigText_Printf(configFile, "Option_LanguageSet=%s\n", BoolToString(aConfig->osNvramConfig.ncd.option.input_language));
    SIM_ConfigText_Printf(configFile, "Option_RTCSet=%s\n", BoolToString(aConfig->osNvramConfig.ncd.option.input_rtc));
    SIM_ConfigText_Printf(configFile, "Option_NicknameSet=%s\n", BoolToString(aConfig->osNvramConfig.ncd.option.input_nickname));
    SIM_ConfigText_Printf(configFile, "Option_Timezone=%d\n", aConfig->osNvramConfig.ncd.option.timezone);
    SIM_ConfigText_Printf(configFile, "Option_RTCClockAdjust=%d\n", aConfig->osNvramConfig.ncd.option.rtcClockAdjust);
    SIM_ConfigText_Printf(configFile, "Option_RTCOffset=%lld\n", (long long)aConfig->osNvramConfig.ncd.option.rtcOffset);
    SIM_ConfigText_Printf(configFile, "\n");
    SIM_ConfigText_Printf(configFile, "[LAN]\n");
    SIM_ConfigText_Printf(configFile, "ServerIp=%s\n", aConfig->netSettings.serverIp);
    SIM_ConfigText_Printf(configFile, "\n");
}

// Save a SIM_config_type to the configuration file
void SIM_Config_SaveConfigFile(SIM_config_type * aConfig)
{
    SIM_ConfigText text = { 0 };
    WriteConfig(&text, aConfig);
    SIM_ConfigText_Save(&text, SIM_CONFIG_FILE_PATH);
    SIM_ConfigText_Free(&text);
}

void SIM_Config_SetOSNVRAMConfig(SIM_config_type * aConfig)
{
    MI_CpuCopy8(&aConfig->osNvramConfig, (void*)OS_GetSystemWork()->nvramUserInfo, sizeof(NVRAMConfig));
    
}
