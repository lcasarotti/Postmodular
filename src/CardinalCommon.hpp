/*
 * DISTRHO Cardinal Plugin
 * Copyright (C) 2021-2024 Filipe Coelho <falktx@falktx.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "DistrhoUtils.hpp"

#include <string>

extern const std::string CARDINAL_VERSION;

struct CardinalPluginContext;

// -----------------------------------------------------------------------------------------------------------

namespace rack {

namespace ui {
struct Menu;
}

namespace window {
void generateScreenshot();
}

bool isMini();
bool isStandalone();

#ifdef ARCH_WIN
enum SpecialPath {
    kSpecialPathUserProfile,
    kSpecialPathCommonProgramFiles,
    kSpecialPathProgramFiles,
    kSpecialPathAppData,
    kSpecialPathMyDocuments,
};
std::string getSpecialPath(SpecialPath type);
#endif

#ifdef DISTRHO_OS_WASM
extern char* patchFromURL;
extern char* patchRemoteURL;
extern char* patchStorageSlug;
void syncfs();
#endif

std::string homeDir();

void switchDarkMode(bool darkMode);

} // namespace rack

// -----------------------------------------------------------------------------------------------------------

namespace patchUtils {

void loadDialog();
void loadPathDialog(const std::string& path, bool asTemplate = false);
void loadSelectionDialog();
void loadTemplate(bool factory);
void loadTemplateDialog(bool factory);
void revertDialog();
void saveDialog(const std::string& path);
void saveAsDialog();
void saveAsDialogUncompressed();
void saveTemplateDialog();
void appendSelectionContextMenu(rack::ui::Menu* menu);
void openBrowser(const std::string& url);

} // namespace patchUtils

// -----------------------------------------------------------------------------------------------------------

#if defined(HAVE_LIBLO) && defined(HEADLESS)
# define CARDINAL_INIT_OSC_THREAD
#endif

#ifdef HAVE_LIBLO
# include <lo/lo_types.h>
#endif

START_NAMESPACE_DISTRHO

class CardinalBasePlugin;
class CardinalBaseUI;

struct Initializer
{
    std::string templatePath;
    std::string factoryTemplatePath;
    bool shouldSaveSettings = false;
    bool pluginsInitialized = false;

    Initializer(const CardinalBasePlugin* plugin, const CardinalBaseUI* ui);
    ~Initializer();
    void loadSettings(bool isRealInstance);
    // Lazy: loads Rack static plugins + browser DB. Safe to call multiple times.
    // Must be called before loadTemplate(). Skipped for dummy/scan instances.
    void ensurePluginsLoaded();

  #ifdef HAVE_LIBLO
    lo_server oscServer = nullptr;
   #ifdef CARDINAL_INIT_OSC_THREAD
    lo_server_thread oscServerThread = nullptr;
   #endif
    CardinalBasePlugin* remotePluginInstance = nullptr;

    bool startRemoteServer(const char* port);
    void stopRemoteServer();
    void stepRemoteServer();
  #endif

  #ifdef CARDINAL_ACCESSIBLE_HTTP
    CardinalBasePlugin* httpPluginInstance = nullptr;
    void startHttpServer();
    void stopHttpServer();
    void processPendingHttpRequests(const std::string& autosavePath, CardinalPluginContext* ctx);
  #endif
};

#ifdef CARDINAL_ACCESSIBLE_HTTP
// Called from CardinalUI::uiIdle() to safely load a pending patch on the UI thread.
void httpProcessPendingPatchFromUI(CardinalPluginContext* ctx);
#endif

#ifndef HEADLESS
void handleHostParameterDrag(const CardinalPluginContext* pcontext, uint index, bool started);
#endif

END_NAMESPACE_DISTRHO

// -----------------------------------------------------------------------------------------------------------
