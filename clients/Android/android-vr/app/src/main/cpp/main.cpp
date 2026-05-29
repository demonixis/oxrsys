// SPDX-License-Identifier: MPL-2.0

#include <android/log.h>
#include <android_native_app_glue.h>

#include "XrApp.h"

#define LOG_TAG "OXRSys-Android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct AppState
{
    oxr::XrApp xrApp;
    bool resumed = false;
};

static void app_handle_cmd(struct android_app* app, int32_t cmd)
{
    auto* state = (AppState*)app->userData;

    switch (cmd)
    {
    case APP_CMD_START:
        LOGI("APP_CMD_START");
        break;
    case APP_CMD_RESUME:
        LOGI("APP_CMD_RESUME");
        state->resumed = true;
        break;
    case APP_CMD_PAUSE:
        LOGI("APP_CMD_PAUSE");
        state->resumed = false;
        break;
    case APP_CMD_STOP:
        LOGI("APP_CMD_STOP");
        break;
    case APP_CMD_DESTROY:
        LOGI("APP_CMD_DESTROY");
        break;
    case APP_CMD_INIT_WINDOW:
        LOGI("APP_CMD_INIT_WINDOW");
        break;
    case APP_CMD_TERM_WINDOW:
        LOGI("APP_CMD_TERM_WINDOW");
        break;
    default:
        break;
    }
}

void android_main(struct android_app* app)
{
    LOGI("OXRSys Android starting...");

    AppState appState;

    if (!appState.xrApp.Initialize(app))
    {
        LOGE("Failed to initialize OpenXR");
        return;
    }

    // Set up app command handler AFTER OpenXR init (matching Meta SDK pattern)
    app->userData = &appState;
    app->onAppCmd = app_handle_cmd;

    LOGI("OpenXR initialized, entering main loop");

    while (!app->destroyRequested)
    {
        // Process Android events
        // Block when not resumed and session not active (saves battery)
        // Following the exact pattern from Meta's XrCompositor_NativeActivity sample
        for (;;)
        {
            int events;
            struct android_poll_source* source;

            const int timeoutMilliseconds =
                (!appState.resumed && !appState.xrApp.IsSessionActive() &&
                 !app->destroyRequested)
                    ? -1
                    : 0;

            if (ALooper_pollOnce(timeoutMilliseconds, nullptr, &events, (void**)&source) < 0)
            {
                break;
            }

            if (source != nullptr)
            {
                source->process(app, source);
            }
        }

        // Run one XR frame (handles xrPollEvent + rendering)
        appState.xrApp.RunFrame();
    }

    appState.xrApp.Shutdown();
    LOGI("OXRSys Android stopped");
}
