#include "twitchdockwidget.h"

#include <QDockWidget>

extern "C" {
#include <obs-frontend-api.h>
#include <obs-module.h>
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obstwitchplugin", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "OBS Twitch integration boilerplate: chat, stream metadata, and friend streaming links.";
}

static QDockWidget *g_obsDock = nullptr;

bool obs_module_load(void)
{
    // Create a standard Qt dock and register it in OBS frontend UI.
    g_obsDock = new QDockWidget(QStringLiteral("Twitch Integration"));
    g_obsDock->setObjectName(QStringLiteral("obstwitchplugin_dock"));
    g_obsDock->setWidget(new TwitchDockWidget(g_obsDock));

    obs_frontend_add_dock(g_obsDock);
    return true;
}

void obs_module_unload(void)
{
    if (g_obsDock) {
        g_obsDock->deleteLater();
        g_obsDock = nullptr;
    }
}
