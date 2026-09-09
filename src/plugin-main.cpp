#include "twitchdockwidget.h"

#include <QDockWidget>

extern "C" {
#if __has_include(<obs/obs-frontend-api.h>)
#include <obs/obs-frontend-api.h>
#else
#include <obs-frontend-api.h>
#endif
#include <obs-module.h>
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obstwitchplugin", "en-US")

namespace {
constexpr auto kDockTitle = "Twitch Integration";
constexpr auto kDockId = "obstwitchplugin_dock";
}

MODULE_EXPORT const char *obs_module_description(void)
{
    return "OBS Twitch integration boilerplate: chat, stream metadata, and friend streaming links.";
}

static QDockWidget *g_obsDock = nullptr;

bool obs_module_load(void)
{
    // Create a standard Qt dock and register it in OBS frontend UI.
    g_obsDock = new QDockWidget(QString::fromLatin1(kDockTitle));
    g_obsDock->setObjectName(QString::fromLatin1(kDockId));
    g_obsDock->setWidget(new TwitchDockWidget(g_obsDock));

    if (!obs_frontend_add_custom_qdock(kDockId, g_obsDock)) {
        delete g_obsDock;
        g_obsDock = nullptr;
        return false;
    }

    return true;
}

void obs_module_unload(void)
{
    if (g_obsDock) {
        obs_frontend_remove_dock(kDockId);
        g_obsDock->deleteLater();
        g_obsDock = nullptr;
    }
}
