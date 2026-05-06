/*
 * DISTRHO Cardinal Plugin
 * Copyright (C) 2021-2026 Filipe Coelho <falktx@falktx.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * This file is partially based on VCVRack's patch.cpp
 * Copyright (C) 2016-2021 VCV.
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 */

#include "CardinalCommon.hpp"

#include "AsyncDialog.hpp"
#include "CardinalPluginContext.hpp"
#include "DistrhoPluginUtils.hpp"

#include <asset.hpp>
#include <context.hpp>
#include <history.hpp>
#include <patch.hpp>
#include <settings.hpp>
#include <string.hpp>
#include <system.hpp>
#include <app/Browser.hpp>
#include <app/Scene.hpp>
#include <engine/Engine.hpp>
#include <window/Window.hpp>

#ifndef DISTRHO_PLUGIN_WANT_DIRECT_ACCESS
# error wrong build
#endif

#if (defined(STATIC_BUILD) && !defined(__MOD_DEVICES__)) || CARDINAL_VARIANT_LOADER || CARDINAL_VARIANT_MINI
# undef CARDINAL_INIT_OSC_THREAD
#endif

#ifdef NDEBUG
# undef DEBUG
#endif

// for finding special paths
#ifdef ARCH_WIN
# include <shlobj.h>
#else
# include <pwd.h>
# include <unistd.h>
#endif

#ifdef ARCH_LIN
# include <fstream>
#endif

#ifdef HAVE_LIBLO
# include <lo/lo.h>
#endif

#ifdef DISTRHO_OS_WASM
# include <emscripten/emscripten.h>
#endif

#if defined(CARDINAL_COMMON_DSP_ONLY) || defined(HEADLESS)
# define HEADLESS_BEHAVIOUR
#endif

#if CARDINAL_VARIANT_FX
# define CARDINAL_VARIANT_NAME "fx"
#elif CARDINAL_VARIANT_LOADER
# define CARDINAL_VARIANT_NAME "loader"
#elif CARDINAL_VARIANT_MINI
# define CARDINAL_VARIANT_NAME "mini"
#elif CARDINAL_VARIANT_NATIVE
# define CARDINAL_VARIANT_NAME "native"
#elif CARDINAL_VARIANT_SYNTH
# define CARDINAL_VARIANT_NAME "synth"
#else
# define CARDINAL_VARIANT_NAME "main"
#endif

#ifdef DISTRHO_OS_WASM
# if CARDINAL_VARIANT_MINI
#  define CARDINAL_WASM_WELCOME_TEMPLATE_FILENAME "welcome-wasm-mini"
# else
#  define CARDINAL_WASM_WELCOME_TEMPLATE_FILENAME "welcome-wasm"
# endif
#endif

namespace rack {
namespace asset {
std::string patchesPath();
void destroy();
}
namespace plugin {
void initStaticPlugins();
void destroyStaticPlugins();
}
}

const std::string CARDINAL_VERSION = "26.02";

// -----------------------------------------------------------------------------------------------------------

#ifndef HEADLESS
void handleHostParameterDrag(const CardinalPluginContext* pcontext, uint index, bool started)
{
    DISTRHO_SAFE_ASSERT_RETURN(pcontext->ui != nullptr,);

   #ifndef CARDINAL_COMMON_DSP_ONLY
    if (started)
    {
        pcontext->ui->editParameter(index, true);
        pcontext->ui->setParameterValue(index, pcontext->parameters[index]);
    }
    else
    {
        pcontext->ui->editParameter(index, false);
    }
   #endif
}
#endif

// --------------------------------------------------------------------------------------------------------------------

CardinalPluginContext::CardinalPluginContext(Plugin* const p)
   #if CARDINAL_VARIANT_FX
    : variant(kCardinalVariantFX),
   #elif CARDINAL_VARIANT_LOADER
    : variant(kCardinalVariantLoader),
   #elif CARDINAL_VARIANT_MAIN
    : variant(kCardinalVariantMain),
   #elif CARDINAL_VARIANT_MINI
    : variant(kCardinalVariantMini),
   #elif CARDINAL_VARIANT_NATIVE
    : variant(kCardinalVariantNative),
   #elif CARDINAL_VARIANT_SYNTH
    : variant(kCardinalVariantSynth),
   #else
    #error cardinal variant not set
   #endif
      parameterCount(CARDINAL_NUM_PARAMETERS),
      parameters(new float[CARDINAL_NUM_PARAMETERS]),
      bufferSize(p != nullptr ? p->getBufferSize() : 0),
      processCounter(0),
      sampleRate(p != nullptr ? p->getSampleRate() : 0.0),
      bypassed(false),
      playing(false),
      reset(false),
      bbtValid(false),
      bar(1),
      beat(1),
      beatsPerBar(4),
      beatType(4),
      frame(0),
      barStartTick(0.0),
      beatsPerMinute(120.0),
      tick(0.0),
      tickClock(0.0),
      ticksPerBeat(0.0),
      ticksPerClock(0.0),
      ticksPerFrame(0.0),
      nativeWindowId(0),
      dataIns(nullptr),
      dataOuts(nullptr),
      midiEvents(nullptr),
      midiEventCount(0),
      plugin(p),
      tlw(nullptr),
      ui(nullptr)
{
    std::memset(parameters, 0, sizeof(float) * CARDINAL_NUM_PARAMETERS);
}

bool CardinalPluginContext::addIdleCallback(IdleCallback* const cb) const
{
   #if !(defined(HEADLESS) || defined(CARDINAL_COMMON_DSP_ONLY))
    if (ui != nullptr)
    {
        ui->addIdleCallback(cb);
        return true;
    }
   #else
    // unused
    (void)cb;
   #endif

    return false;
}

void CardinalPluginContext::removeIdleCallback(IdleCallback* const cb) const
{
   #if !(defined(HEADLESS) || defined(CARDINAL_COMMON_DSP_ONLY))
    if (ui != nullptr)
        ui->removeIdleCallback(cb);
   #else
    // unused
    (void)cb;
   #endif
}

void CardinalPluginContext::writeMidiMessage(const rack::midi::Message& message, const uint8_t channel)
{
    if (bypassed)
        return;

    const size_t size = message.bytes.size();
    DISTRHO_SAFE_ASSERT_RETURN(size > 0,);
    DISTRHO_SAFE_ASSERT_RETURN(message.frame >= 0,);

    MidiEvent event;
    event.frame = message.frame;

    switch (message.bytes[0] & 0xF0)
    {
    case 0x80:
    case 0x90:
    case 0xA0:
    case 0xB0:
    case 0xE0:
        event.size = 3;
        break;
    case 0xC0:
    case 0xD0:
        event.size = 2;
        break;
    case 0xF0:
        switch (message.bytes[0] & 0x0F)
        {
        case 0x0:
        case 0x4:
        case 0x5:
        case 0x7:
        case 0x9:
        case 0xD:
            // unsupported
            return;
        case 0x1:
        case 0x2:
        case 0x3:
        case 0xE:
            event.size = 3;
            break;
        case 0x6:
        case 0x8:
        case 0xA:
        case 0xB:
        case 0xC:
        case 0xF:
            event.size = 1;
            break;
        }
        break;
    default:
        // invalid
        return;
    }

    DISTRHO_SAFE_ASSERT_RETURN(size >= event.size,);

    std::memcpy(event.data, message.bytes.data(), event.size);

    if (channel != 0 && event.data[0] < 0xF0)
        event.data[0] |= channel & 0x0F;

    plugin->writeMidiEvent(event);
}

// -----------------------------------------------------------------------------------------------------------

namespace rack {
namespace midi {

struct InputQueue::Internal {
    CardinalPluginContext* const pcontext = static_cast<CardinalPluginContext*>(APP);
    const CardinalDISTRHO::MidiEvent* midiEvents = nullptr;
    uint32_t midiEventsLeft = 0;
    uint32_t lastProcessCounter = 0;
    int64_t lastBlockFrame = 0;
};

InputQueue::InputQueue() {
    internal = new Internal;
}

InputQueue::~InputQueue() {
    delete internal;
}

bool InputQueue::tryPop(Message* const messageOut, int64_t maxFrame)
{
    const uint32_t processCounter = internal->pcontext->processCounter;
    const bool processCounterChanged = internal->lastProcessCounter != processCounter;

    if (processCounterChanged)
    {
        internal->lastBlockFrame = internal->pcontext->engine->getBlockFrame();
        internal->lastProcessCounter = processCounter;

        internal->midiEvents = internal->pcontext->midiEvents;
        internal->midiEventsLeft = internal->pcontext->midiEventCount;
    }

    if (internal->midiEventsLeft == 0 || maxFrame < internal->lastBlockFrame)
        return false;

    const uint32_t frame = maxFrame - internal->lastBlockFrame;

    if (frame > internal->midiEvents->frame)
        return false;

    const CardinalDISTRHO::MidiEvent& midiEvent(*internal->midiEvents);

    const uint8_t* data;
    if (midiEvent.size > CardinalDISTRHO::MidiEvent::kDataSize)
    {
        data = midiEvent.dataExt;
        messageOut->bytes.resize(midiEvent.size);
    }
    else
    {
        data = midiEvent.data;
    }

    messageOut->frame = frame;
    std::memcpy(messageOut->bytes.data(), data, midiEvent.size);

    ++internal->midiEvents;
    --internal->midiEventsLeft;
    return true;
}

json_t* InputQueue::toJson() const
{
    return nullptr;
}

void InputQueue::fromJson(json_t* rootJ)
{
}

}
}

// -----------------------------------------------------------------------------------------------------------

START_NAMESPACE_DISTRHO

// -----------------------------------------------------------------------------------------------------------

#ifdef HAVE_LIBLO
static void osc_error_handler(int num, const char* msg, const char* path)
{
    d_stderr("Cardinal OSC Error: code: %i, msg: \"%s\", path: \"%s\")", num, msg, path);
}

static int osc_fallback_handler(const char* const path, const char* const types, lo_arg**, int, lo_message, void*)
{
    d_stderr("Cardinal OSC unhandled message \"%s\" with types \"%s\"", path, types);
    return 0;
}

static int osc_hello_handler(const char*, const char*, lo_arg**, int, const lo_message m, void* const self)
{
    d_stdout("Hello received from OSC, saying hello back to them o/");
    const lo_address source = lo_message_get_source(m);
    const lo_server server = static_cast<Initializer*>(self)->oscServer;

    // send list of features first
   #ifdef CARDINAL_INIT_OSC_THREAD
    lo_send_from(source, server, LO_TT_IMMEDIATE, "/resp", "ss", "features", ":screenshot:");
   #else
    lo_send_from(source, server, LO_TT_IMMEDIATE, "/resp", "ss", "features", "");
   #endif

    // then finally hello reply
    lo_send_from(source, server, LO_TT_IMMEDIATE, "/resp", "ss", "hello", "ok");
    return 0;
}

static int osc_load_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{
    d_debug("osc_load_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 1, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types != nullptr && types[0] == 'b', 0);

    const int32_t size = argv[0]->blob.size;
    DISTRHO_SAFE_ASSERT_RETURN(size > 4, 0);

    const uint8_t* const blob = (uint8_t*)(&argv[0]->blob.data);
    DISTRHO_SAFE_ASSERT_RETURN(blob != nullptr, 0);

    bool ok = false;

    if (CardinalBasePlugin* const plugin = static_cast<Initializer*>(self)->remotePluginInstance)
    {
        CardinalPluginContext* const context = plugin->context;
        std::vector<uint8_t> data(size);
        std::memcpy(data.data(), blob, size);

       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(context);
       #endif

        rack::system::removeRecursively(context->patch->autosavePath);
        rack::system::createDirectories(context->patch->autosavePath);
        try {
            rack::system::unarchiveToDirectory(data, context->patch->autosavePath);
            context->patch->loadAutosave();
            ok = true;
        }
        catch (rack::Exception& e) {
            WARN("%s", e.what());
        }

       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(nullptr);
       #endif
    }

    const lo_address source = lo_message_get_source(m);
    const lo_server server = static_cast<Initializer*>(self)->oscServer;
    lo_send_from(source, server, LO_TT_IMMEDIATE, "/resp", "ss", "load", ok ? "ok" : "fail");
    return 0;
}

static int osc_param_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{
    d_debug("osc_param_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 3, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types != nullptr, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[0] == 'h', 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[1] == 'i', 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[2] == 'f', 0);

    if (CardinalBasePlugin* const plugin = static_cast<Initializer*>(self)->remotePluginInstance)
    {
        CardinalPluginContext* const context = plugin->context;

        const int64_t moduleId = argv[0]->h;
        const int paramId = argv[1]->i;
        const float paramValue = argv[2]->f;

       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(context);
       #endif

        rack::engine::Module* const module = context->engine->getModule(moduleId);
        DISTRHO_SAFE_ASSERT_RETURN(module != nullptr, 0);

        context->engine->setParamValue(module, paramId, paramValue);

       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(nullptr);
       #endif
    }

    return 0;
}

static int osc_host_param_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{
    d_debug("osc_host_param_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 2, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types != nullptr, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[0] == 'i', 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[1] == 'f', 0);

    if (CardinalBasePlugin* const plugin = static_cast<Initializer*>(self)->remotePluginInstance)
    {
        CardinalPluginContext* const context = plugin->context;

        const int paramId = argv[0]->i;
        DISTRHO_SAFE_ASSERT_RETURN(paramId >= 0, 0);

        const uint uparamId = static_cast<uint>(paramId);
        DISTRHO_SAFE_ASSERT_UINT2_RETURN(uparamId < kModuleParameterCount, uparamId, kModuleParameterCount, 0);

        const float paramValue = argv[1]->f;

        context->parameters[uparamId] = paramValue;
    }

    return 0;
}

# ifdef CARDINAL_INIT_OSC_THREAD
static int osc_screenshot_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{
    d_debug("osc_screenshot_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 1, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types != nullptr && types[0] == 'b', 0);

    const int32_t size = argv[0]->blob.size;
    DISTRHO_SAFE_ASSERT_RETURN(size > 4, 0);

    const uint8_t* const blob = (uint8_t*)(&argv[0]->blob.data);
    DISTRHO_SAFE_ASSERT_RETURN(blob != nullptr, 0);

    bool ok = false;

    if (CardinalBasePlugin* const plugin = static_cast<Initializer*>(self)->remotePluginInstance)
    {
        if (char* const screenshot = String::asBase64(blob, size).getAndReleaseBuffer())
        {
            ok = plugin->updateStateValue("screenshot", screenshot);
            std::free(screenshot);
        }
    }

    const lo_address source = lo_message_get_source(m);
    const lo_server server = static_cast<Initializer*>(self)->oscServer;
    lo_send_from(source, server, LO_TT_IMMEDIATE, "/resp", "ss", "screenshot", ok ? "ok" : "fail");
    return 0;
}
# endif
#endif

// -----------------------------------------------------------------------------------------------------------

#if defined(DISTRHO_OS_WASM) && !defined(CARDINAL_COMMON_UI_ONLY)
static void WebBrowserDataLoaded(void* const data)
{
    static_cast<Initializer*>(data)->loadSettings(true);
}
#endif

// -----------------------------------------------------------------------------------------------------------

Initializer::Initializer(const CardinalBasePlugin* const plugin, const CardinalBaseUI* const ui)
{
    using namespace rack;

    // Cardinal default settings, potentially overriding VCV Rack ones
    settings::allowCursorLock = false;
    settings::tooltips = true;
    settings::cableOpacity = 0.5f;
    settings::cableTension = 0.75f;
    settings::rackBrightness = 1.0f;
    settings::haloBrightness = 0.25f;
    settings::knobMode = settings::KNOB_MODE_LINEAR;
    settings::knobScroll = false;
    settings::knobScrollSensitivity = 0.001f;
    settings::lockModules = false;
    settings::browserSort = settings::BROWSER_SORT_UPDATED;
    settings::browserZoom = -1.f;
    settings::invertZoom = false;
    settings::squeezeModules = true;
    settings::preferDarkPanels = true;
    settings::uiTheme = "dark";

    // runtime behaviour
    settings::devMode = true;
    settings::isPlugin = true;
   #ifdef HEADLESS_BEHAVIOUR
    settings::headless = true;
   #endif

    // copied from https://community.vcvrack.com/t/16-colour-cable-palette/15951
    settings::cableColors = {
        color::fromHexString("#ff5252"),
        color::fromHexString("#ff9352"),
        color::fromHexString("#ffd452"),
        color::fromHexString("#e8ff52"),
        color::fromHexString("#a8ff52"),
        color::fromHexString("#67ff52"),
        color::fromHexString("#52ff7d"),
        color::fromHexString("#52ffbe"),
        color::fromHexString("#52ffff"),
        color::fromHexString("#52beff"),
        color::fromHexString("#527dff"),
        color::fromHexString("#6752ff"),
        color::fromHexString("#a852ff"),
        color::fromHexString("#e952ff"),
        color::fromHexString("#ff52d4"),
        color::fromHexString("#ff5293"),
    };

    system::init();
    logger::init();
    random::init();
    ui::init();

   #ifdef CARDINAL_COMMON_UI_ONLY
    constexpr const bool isRealInstance = true;
   #else
    const bool isRealInstance = !plugin->isDummyInstance();
   #endif

    if (asset::systemDir.empty())
    {
        if (const char* const bundlePath = (plugin != nullptr ? plugin->getBundlePath() :
                                           #if DISTRHO_PLUGIN_HAS_UI
                                            ui != nullptr ? ui->getBundlePath() :
                                           #endif
                                            nullptr))
        {
            if (const char* const resourcePath = getResourcePath(bundlePath))
            {
                asset::systemDir = resourcePath;
                asset::bundlePath = system::join(asset::systemDir, "PluginManifests");
            }
        }

        if (asset::systemDir.empty() || ! system::exists(asset::systemDir) || ! system::exists(asset::bundlePath))
        {
           #ifdef CARDINAL_PLUGIN_SOURCE_DIR
            // Make system dir point to source code location as fallback
            asset::systemDir = CARDINAL_PLUGIN_SOURCE_DIR DISTRHO_OS_SEP_STR "Rack";
            asset::bundlePath.clear();

            // If source code dir does not exist use install target prefix as system dir
            if (!system::exists(system::join(asset::systemDir, "res")))
           #endif
            {
               #if defined(DISTRHO_OS_WASM)
                asset::systemDir = "/resources";
               #elif defined(ARCH_MAC)
                asset::systemDir = "/Library/Application Support/Cardinal";
               #elif defined(ARCH_WIN)
                asset::systemDir = system::join(getSpecialPath(kSpecialPathCommonProgramFiles), "Postmodular");
               #else
                asset::systemDir = CARDINAL_PLUGIN_PREFIX "/share/cardinal";
               #endif
                asset::bundlePath = system::join(asset::systemDir, "PluginManifests");
            }
        }
    }

    if (asset::userDir.empty())
    {
        asset::userDir = system::join(getSpecialDir(kSpecialDirDocuments), "Cardinal");

        if (isRealInstance)
        {
           #if defined(DISTRHO_OS_WASM) && !defined(CARDINAL_COMMON_UI_ONLY)
            EM_ASM({
                Module.FS.mkdir('/userfiles');
                Module.FS.mount(Module.IDBFS, {}, '/userfiles');
                Module.FS.syncfs(true, function(err) { if (!err) { dynCall('vi', $0, [$1]) } });
            }, WebBrowserDataLoaded, this);
           #else
            system::createDirectory(asset::userDir);
           #endif
        }
    }

   #ifndef CARDINAL_COMMON_DSP_ONLY
    if (asset::configDir.empty())
    {
        asset::configDir = system::join(getSpecialDir(kSpecialDirConfig), "Cardinal");

        if (isRealInstance)
            system::createDirectory(asset::configDir);
    }
   #endif
   
    if (settings::settingsPath.empty())
        settings::settingsPath = asset::config(CARDINAL_VARIANT_NAME ".json");

    templatePath = asset::user("templates/" CARDINAL_VARIANT_NAME ".vcv");
   #ifdef DISTRHO_OS_WASM
    factoryTemplatePath = system::join(asset::patchesPath(), CARDINAL_WASM_WELCOME_TEMPLATE_FILENAME ".vcv");
   #else
    factoryTemplatePath = system::join(asset::patchesPath(), "templates/" CARDINAL_VARIANT_NAME ".vcv");
   #endif

    // Log environment
    INFO("%s %s %s, compatible with Rack version %s", APP_NAME.c_str(), APP_EDITION.c_str(), CARDINAL_VERSION.c_str(), APP_VERSION.c_str());
    INFO("%s", system::getOperatingSystemInfo().c_str());
    INFO("Binary filename: %s", getBinaryFilename());
    if (plugin != nullptr) {
        INFO("Bundle path: %s", plugin->getBundlePath());
   #if DISTRHO_PLUGIN_HAS_UI
    } else if (ui != nullptr) {
        INFO("Bundle path: %s", ui->getBundlePath());
   #endif
    }
    INFO("System directory: %s", asset::systemDir.c_str());
    INFO("User directory: %s", asset::userDir.c_str());
    INFO("Template patch: %s", templatePath.c_str());
    INFO("System template patch: %s", factoryTemplatePath.c_str());

    // Report to user if something is wrong with the installation
    if (asset::systemDir.empty())
    {
        d_stderr2("Failed to locate Cardinal plugin bundle.\n"
                    "Install Cardinal with its bundle folder intact and try again.");
    }
    else if (! system::exists(asset::systemDir))
    {
        d_stderr2("System directory \"%s\" does not exist.\n"
                    "Make sure Cardinal was downloaded and installed correctly.", asset::systemDir.c_str());
    }

    // For dummy/scan instances, defer heavy plugin loading to first real instance.
    // This allows Reaper's plugin scan to complete fast without crashing.
    if (isRealInstance)
        ensurePluginsLoaded();

    loadSettings(isRealInstance);

   #if defined(CARDINAL_INIT_OSC_THREAD)
    INFO("Initializing OSC Remote control");
    const char* port;
    if (const char* const portEnv = std::getenv("CARDINAL_REMOTE_HOST_PORT"))
        port = portEnv;
    else
        port = CARDINAL_DEFAULT_REMOTE_PORT;
    startRemoteServer(port);
   #elif defined(HAVE_LIBLO)
    if (isStandalone()) {
        INFO("OSC Remote control is available on request");
    } else {
        INFO("OSC Remote control is not available on plugin variants");
    }
   #else
    INFO("OSC Remote control is not enabled in this build");
   #endif
}

Initializer::~Initializer()
{
    using namespace rack;

   #ifdef HAVE_LIBLO
    stopRemoteServer();
   #endif

    if (shouldSaveSettings)
    {
        INFO("Save settings");
        settings::save();
    }

    INFO("Clearing asset paths");
    asset::bundlePath.clear();
    asset::systemDir.clear();
    asset::userDir.clear();

    INFO("Destroying plugins");
    plugin::destroyStaticPlugins();

    INFO("Destroying colourized assets");
    asset::destroy();

    INFO("Destroying settings");
    settings::destroy();

    INFO("Destroying logger");
    logger::destroy();
}

void Initializer::ensurePluginsLoaded()
{
    using namespace rack;

    if (pluginsInitialized)
        return;
    pluginsInitialized = true;

    INFO("Initializing plugins");
    plugin::initStaticPlugins();

    INFO("Initializing plugin browser DB");
    app::browserInit();
}

void Initializer::loadSettings(const bool isRealInstance)
{
    using namespace rack;

    if (isRealInstance)
    {
        INFO("Loading settings");
        settings::load();
        shouldSaveSettings = true;
    }

    // enforce settings that do not make sense as anything else
    settings::safeMode = false;
    settings::token.clear();
    settings::windowMaximized = false;
    settings::windowPos = math::Vec(0, 0);
    settings::pixelRatio = 0.0;
    settings::sampleRate = 0;
    settings::threadCount = 1;
    settings::autosaveInterval = 0;
    settings::skipLoadOnLaunch = true;
    settings::autoCheckUpdates = false;
    settings::showTipsOnLaunch = false;
    settings::tipIndex = -1;

    if (settings::uiTheme != "dark" && settings::uiTheme != "light")
    {
        settings::uiTheme = "dark";
        rack::ui::refreshTheme();
    }

    // reload dark/light mode as necessary
    switchDarkMode(settings::uiTheme == "dark");
}

#ifdef HAVE_LIBLO
bool Initializer::startRemoteServer(const char* const port)
{
   #ifdef CARDINAL_INIT_OSC_THREAD
    if (oscServerThread != nullptr)
        return true;

    if ((oscServerThread = lo_server_thread_new_with_proto(port, LO_UDP, osc_error_handler)) == nullptr)
        return false;

    oscServer = lo_server_thread_get_server(oscServerThread);

    lo_server_thread_add_method(oscServerThread, "/hello", "", osc_hello_handler, this);
    lo_server_thread_add_method(oscServerThread, "/host-param", "if", osc_host_param_handler, this);
    lo_server_thread_add_method(oscServerThread, "/load", "b", osc_load_handler, this);
    lo_server_thread_add_method(oscServerThread, "/param", "hif", osc_param_handler, this);
    lo_server_thread_add_method(oscServerThread, "/screenshot", "b", osc_screenshot_handler, this);
    lo_server_thread_add_method(oscServerThread, nullptr, nullptr, osc_fallback_handler, nullptr);
    lo_server_thread_start(oscServerThread);
   #else
    if (oscServer != nullptr)
        return true;

    if ((oscServer = lo_server_new_with_proto(port, LO_UDP, osc_error_handler)) == nullptr)
        return false;

    lo_server_add_method(oscServer, "/hello", "", osc_hello_handler, this);
    lo_server_add_method(oscServer, "/host-param", "if", osc_host_param_handler, this);
    lo_server_add_method(oscServer, "/load", "b", osc_load_handler, this);
    lo_server_add_method(oscServer, "/param", "hif", osc_param_handler, this);
    lo_server_add_method(oscServer, nullptr, nullptr, osc_fallback_handler, nullptr);
   #endif

    return true;
}

void Initializer::stopRemoteServer()
{
    DISTRHO_SAFE_ASSERT(remotePluginInstance == nullptr);

   #ifdef CARDINAL_INIT_OSC_THREAD
    if (oscServerThread != nullptr)
    {
        lo_server_thread_stop(oscServerThread);
        lo_server_thread_del_method(oscServerThread, nullptr, nullptr);
        lo_server_thread_free(oscServerThread);
        oscServerThread = nullptr;
        oscServer = nullptr;
    }
   #else
    if (oscServer != nullptr)
    {
        lo_server_del_method(oscServer, nullptr, nullptr);
        lo_server_free(oscServer);
        oscServer = nullptr;
    }
   #endif
}

void Initializer::stepRemoteServer()
{
    DISTRHO_SAFE_ASSERT_RETURN(oscServer != nullptr,);
    DISTRHO_SAFE_ASSERT_RETURN(remotePluginInstance != nullptr,);

   #ifndef CARDINAL_INIT_OSC_THREAD
    for (;;)
    {
        try {
            if (lo_server_recv_noblock(oscServer, 0) == 0)
                break;
        } DISTRHO_SAFE_EXCEPTION_CONTINUE("stepRemoteServer")
    }
   #endif
}
#endif // HAVE_LIBLO

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DISTRHO

// --------------------------------------------------------------------------------------------------------------------

namespace rack {

bool isMini()
{
#if CARDINAL_VARIANT_MINI
    return true;
#else
    return false;
#endif
}

bool isStandalone()
{
    static const bool standalone = std::strstr(getPluginFormatName(), "Standalone") != nullptr;
    return standalone;
}

#ifdef ARCH_WIN
std::string getSpecialPath(const SpecialPath type)
{
    int csidl;
    switch (type)
    {
    case kSpecialPathUserProfile:
        csidl = CSIDL_PROFILE;
        break;
    case kSpecialPathCommonProgramFiles:
        csidl = CSIDL_PROGRAM_FILES_COMMON;
        break;
    case kSpecialPathProgramFiles:
        csidl = CSIDL_PROGRAM_FILES;
        break;
    case kSpecialPathAppData:
        csidl = CSIDL_APPDATA;
        break;
    case kSpecialPathMyDocuments:
        csidl = CSIDL_MYDOCUMENTS;
        break;
    default:
        return {};
    }

    WCHAR path[MAX_PATH] = {};

    if (SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, path) == S_OK)
        return string::UTF16toUTF8(path);

    return {};
}
#endif

#ifdef DISTRHO_OS_WASM
char* patchFromURL = nullptr;
char* patchRemoteURL = nullptr;
char* patchStorageSlug = nullptr;

void syncfs()
{
    settings::save();

   #ifndef CARDINAL_COMMON_UI_ONLY
    EM_ASM({
        Module.FS.syncfs(false, function(){} );
    });
   #endif
}
#endif

std::string homeDir()
{
   #ifdef ARCH_WIN
    return getSpecialPath(kSpecialPathUserProfile);
   #else
    if (const char* const home = getenv("HOME"))
        return home;
    if (struct passwd* const pwd = getpwuid(getuid()))
        return pwd->pw_dir;
   #endif
    return {};
}

} // namespace rack

// --------------------------------------------------------------------------------------------------------------------

namespace patchUtils
{

using namespace rack;

#ifndef HEADLESS_BEHAVIOUR
static void promptClear(const char* const message, const std::function<void()> action)
{
    if (APP->history->isSaved() || APP->scene->rack->hasModules())
        return action();

    asyncDialog::create(message, action);
}
#endif

void loadDialog()
{
#ifndef HEADLESS_BEHAVIOUR
    promptClear("The current patch is unsaved. Clear it and open a new patch?", []() {
        std::string dir;
        if (! APP->patch->path.empty())
            dir = system::getDirectory(APP->patch->path);
        else
            dir = homeDir();

        CardinalPluginContext* const pcontext = static_cast<CardinalPluginContext*>(APP);
        DISTRHO_SAFE_ASSERT_RETURN(pcontext != nullptr,);

        CardinalBaseUI* const ui = static_cast<CardinalBaseUI*>(pcontext->ui);
        DISTRHO_SAFE_ASSERT_RETURN(ui != nullptr,);

        DISTRHO_NAMESPACE::FileBrowserOptions opts;
        opts.saving = ui->saving = false;
        opts.startDir = dir.c_str();
        opts.title = "Open patch";
        ui->openFileBrowser(opts);
    });
#endif
}

void loadPathDialog(const std::string& path, const bool asTemplate)
{
#ifndef HEADLESS_BEHAVIOUR
    promptClear("The current patch is unsaved. Clear it and open the new patch?", [path, asTemplate]() {
        APP->patch->loadAction(path);

        if (asTemplate)
        {
            APP->patch->path = "";
            APP->history->setSaved();
        }

       #ifdef DISTRHO_OS_WASM
        syncfs();
       #endif

        if (remoteUtils::RemoteDetails* const remoteDetails = remoteUtils::getRemote())
            if (remoteDetails->autoDeploy)
                remoteUtils::sendFullPatchToRemote(remoteDetails);
    });
#endif
}

void loadSelectionDialog()
{
    app::RackWidget* const w = APP->scene->rack;

    std::string selectionDir = asset::user("selections");
    system::createDirectories(selectionDir);

    async_dialog_filebrowser(false, nullptr, selectionDir.c_str(), "Import selection", [w](char* pathC) {
        if (!pathC) {
            // No path selected
            return;
        }

        try {
            w->loadSelection(pathC);
        }
        catch (Exception& e) {
            async_dialog_message(e.what());
        }

        std::free(pathC);

       #ifdef DISTRHO_OS_WASM
        syncfs();
       #endif

        if (remoteUtils::RemoteDetails* const remoteDetails = remoteUtils::getRemote())
            if (remoteDetails->autoDeploy)
                remoteUtils::sendFullPatchToRemote(remoteDetails);
    });
}

void loadTemplate(const bool factory)
{
    try {
        APP->patch->load(factory ? APP->patch->factoryTemplatePath : APP->patch->templatePath);
    }
    catch (Exception& e) {
        // if user template failed, try the factory one
        if (!factory)
            return loadTemplate(true);

        const std::string message = string::f("Could not load template patch, clearing rack: %s", e.what());
        asyncDialog::create(message.c_str());

        APP->patch->clear();
        APP->patch->clearAutosave();
    }

    // load() sets the patch's original patch, but we don't want to use that.
    APP->patch->path.clear();
    APP->history->setSaved();

   #ifdef DISTRHO_OS_WASM
    syncfs();
   #endif

    if (remoteUtils::RemoteDetails* const remoteDetails = remoteUtils::getRemote())
        if (remoteDetails->autoDeploy)
            remoteUtils::sendFullPatchToRemote(remoteDetails);
}


void loadTemplateDialog(const bool factory)
{
#ifndef HEADLESS_BEHAVIOUR
    promptClear("The current patch is unsaved. Clear it and start a new patch?", [factory]() {
        loadTemplate(factory);
    });
#endif
}

void revertDialog()
{
#ifndef HEADLESS_BEHAVIOUR
    if (APP->patch->path.empty())
        return;
    promptClear("Revert patch to the last saved state?", []{
        APP->patch->loadAction(APP->patch->path);

       #ifdef DISTRHO_OS_WASM
        syncfs();
       #endif

        if (remoteUtils::RemoteDetails* const remoteDetails = remoteUtils::getRemote())
            if (remoteDetails->autoDeploy)
                remoteUtils::sendFullPatchToRemote(remoteDetails);
    });
#endif
}

void saveDialog(const std::string& path)
{
#ifndef HEADLESS_BEHAVIOUR
    if (path.empty()) {
        return;
    }

    // Note: If save() fails below, this should probably be reset. But we need it so toJson() doesn't set the "unsaved" property.
    APP->history->setSaved();

    try {
        APP->patch->save(path);
    }
    catch (Exception& e) {
        asyncDialog::create(string::f("Could not save patch: %s", e.what()).c_str());
        return;
    }

    APP->patch->pushRecentPath(path);

   #ifdef DISTRHO_OS_WASM
    syncfs();
   #else
    rack::settings::save();
   #endif
#endif
}

#ifndef HEADLESS_BEHAVIOUR
static void saveAsDialog(const bool uncompressed)
{
    std::string dir;
    if (! APP->patch->path.empty())
    {
        dir = system::getDirectory(APP->patch->path);
    }
    else
    {
        dir = asset::user("patches");
        system::createDirectories(dir);
    }

    CardinalPluginContext* const pcontext = static_cast<CardinalPluginContext*>(APP);
    DISTRHO_SAFE_ASSERT_RETURN(pcontext != nullptr,);

    CardinalBaseUI* const ui = static_cast<CardinalBaseUI*>(pcontext->ui);
    DISTRHO_SAFE_ASSERT_RETURN(ui != nullptr,);

    DISTRHO_NAMESPACE::FileBrowserOptions opts;
    opts.saving = ui->saving = true;
    opts.defaultName = "patch.vcv";
    opts.startDir = dir.c_str();
    opts.title = "Save patch";
    ui->savingUncompressed = uncompressed;
    ui->openFileBrowser(opts);
}
#endif

void saveAsDialog()
{
#ifndef HEADLESS_BEHAVIOUR
    saveAsDialog(false);
#endif
}

void saveAsDialogUncompressed()
{
#ifndef HEADLESS_BEHAVIOUR
    saveAsDialog(true);
#endif
}

void saveTemplateDialog()
{
    asyncDialog::create("Overwrite template patch?", []{
        rack::system::createDirectories(system::getDirectory(APP->patch->templatePath));

        try {
            APP->patch->save(APP->patch->templatePath);
        }
        catch (Exception& e) {
            asyncDialog::create(string::f("Could not save template patch: %s", e.what()).c_str());
            return;
        }

       #ifdef DISTRHO_OS_WASM
        syncfs();
       #endif
    });
}

void openBrowser(const std::string& url)
{
#ifdef DISTRHO_OS_WASM
    EM_ASM({
        window.open(UTF8ToString($0), '_blank');
    }, url.c_str());
#else
    system::openBrowser(url);
#endif
}

}

// --------------------------------------------------------------------------------------------------------------------

void async_dialog_filebrowser(const bool saving,
                              const char* const defaultName,
                              const char* const startDir,
                              const char* const title,
                              const std::function<void(char* path)> action)
{
#ifndef HEADLESS_BEHAVIOUR
    CardinalPluginContext* const pcontext = static_cast<CardinalPluginContext*>(APP);
    DISTRHO_SAFE_ASSERT_RETURN(pcontext != nullptr,);

    CardinalBaseUI* const ui = static_cast<CardinalBaseUI*>(pcontext->ui);
    DISTRHO_SAFE_ASSERT_RETURN(ui != nullptr,);

    // only 1 dialog possible at a time
    DISTRHO_SAFE_ASSERT_RETURN(ui->filebrowserhandle == nullptr,);

    DISTRHO_NAMESPACE::FileBrowserOptions opts;
    opts.saving = saving;
    opts.defaultName = defaultName;
    opts.startDir = startDir;
    opts.title = title;

    ui->filebrowseraction = action;
    ui->filebrowserhandle = fileBrowserCreate(true, pcontext->nativeWindowId, pcontext->window->pixelRatio, opts);
#endif
}

void async_dialog_message(const char* const message)
{
#ifndef HEADLESS_BEHAVIOUR
    asyncDialog::create(message);
#endif
}

void async_dialog_message(const char* const message, const std::function<void()> action)
{
#ifndef HEADLESS_BEHAVIOUR
    asyncDialog::create(message, action);
#endif
}

void async_dialog_text_input(const char* const message, const char* const text,
                             const std::function<void(char* newText)> action)
{
#ifndef HEADLESS_BEHAVIOUR
    asyncDialog::textInput(message, text, action);
#endif
}

// --------------------------------------------------------------------------------------------------------------------
// Cardinal Accessible HTTP Server
// --------------------------------------------------------------------------------------------------------------------

#ifdef CARDINAL_ACCESSIBLE_HTTP

// httplib requires Windows 10+ APIs; override the global _WIN32_WINNT set by Makefile
#ifdef _WIN32_WINNT
# undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0A00

#include "extra/httplib.h"

#ifdef _WIN32
# include <windows.h>
# include <shellapi.h>
#endif

#include "rack.hpp"
#include "engine/Engine.hpp"
#include "plugin.hpp"

#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <vector>
#include <memory>
#include <cstdlib>
#include <cstring>

START_NAMESPACE_DISTRHO

// Global server and thread owned by Initializer
static httplib::Server*  gHttpServer  = nullptr;
static std::thread       gHttpThread;
static std::atomic<bool> gHttpRunning{false};

// Deferred patch load (HTTP thread → audio thread → UI thread)
static std::atomic<bool>     gPendingPatchLoad{false};
static std::string           gPendingPatchJson;   // for JSON-body load (written via audio thread)
static std::mutex            gPendingPatchMutex;

// Path-based load: HTTP thread → UI thread directly (skips audio thread)
// gPendingPatchPath: .vcv file path; non-empty triggers ctx->patch->load(path) on UI thread
static std::string           gPendingPatchPath;
static std::mutex            gPendingPathMutex;

// Patch load handoff: audio thread writes file, UI thread calls loadAutosave()
static std::atomic<bool>     gPendingPatchFromUI{false};

// Completion signal: HTTP thread waits here for loadAutosave() to finish.
static std::mutex              gPatchSyncMtx;
static std::condition_variable gPatchSyncCV;
static bool                    gPatchSyncDone = false;

// Diagnostics
static std::atomic<uint64_t> gRunCallCount{0};
static std::atomic<uint64_t> gFlagSetCount{0};      // incremented when flag stored true
static std::atomic<uint64_t> gFlagDetectedCount{0}; // incremented when flag seen true in audio thread
static std::string           gLastPatchLoadResult;

// Deferred engine mutations (module/cable add/remove).
// HTTP handlers queue an op here; processPendingHttpRequests() (audio thread,
// between stepBlock() calls, no engine lock held) executes it and signals done.
struct PendingEngineOp {
    enum Type { ADD_MODULE, REMOVE_MODULE, ADD_CABLE, REMOVE_CABLE } type;
    std::string pluginSlug, moduleSlug;           // ADD_MODULE
    int64_t     moduleId    = -1;                 // REMOVE_MODULE
    int64_t     outModuleId = -1, outPortId = -1; // ADD_CABLE
    int64_t     inModuleId  = -1, inPortId  = -1; // ADD_CABLE
    int64_t     cableId     = -1;                 // REMOVE_CABLE
    // Result (written by audio thread)
    int64_t     resultId    = -1;
    std::string error;
    bool        done        = false;
};
static std::mutex               gEngineOpMutex;
static std::condition_variable  gEngineOpCV;
static PendingEngineOp*         gPendingEngineOp = nullptr;

// Serialises raw module/cable pointer access against loadAutosave().
// HTTP handlers and loadAutosave() both hold this mutex to prevent
// use-after-free when patch reload deletes modules while WS thread reads them.
static std::mutex               gModuleAccessMutex;

// Audio device list and active config -- populated by RtAudioBridge::open() on Windows native.
// Remain empty/default on VST3/CLAP (where audio is managed by the host).
extern "C" {
char gCardinalAudioDevicesJson[32768] = "[]";
char gCardinalAudioConfigJson[512]    = "{}";
}

// Save key=value config file to %APPDATA%\Cardinal\audio.cfg
static bool saveAudioConfigFile(const std::string& driver, const std::string& device,
                                uint32_t sampleRate, uint32_t bufferSize)
{
#ifdef _WIN32
    const char* const appdata = std::getenv("APPDATA");
    if (appdata == nullptr) return false;
    const std::string dir = std::string(appdata) + "\\Cardinal";
    rack::system::createDirectories(dir);
    const std::string path = dir + "\\audio.cfg";
    FILE* const f = fopen(path.c_str(), "w");
    if (f == nullptr) return false;
    fprintf(f, "driver=%s\n", driver.c_str());
    fprintf(f, "device=%s\n", device.c_str());
    fprintf(f, "samplerate=%u\n", sampleRate);
    fprintf(f, "buffersize=%u\n", bufferSize);
    fclose(f);
    return true;
#else
    return false;
    (void)driver; (void)device; (void)sampleRate; (void)bufferSize;
#endif
}

static const int kDefaultHttpPort = 2229;

// ---------------------------------------------------------------------------
// Helpers

static std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

static void setCorsHeaders(httplib::Response& res)
{
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
}

// Serialize a float to JSON — replaces inf/nan (invalid JSON) with null
static std::string floatToJson(float v)
{
    if (std::isinf(v) || std::isnan(v)) return "null";
    return std::to_string(v);
}

// Minimal JSON number extractor — avoids pulling in nlohmann/rapidjson
static double extractNum(const std::string& body, const char* key)
{
    std::string k = std::string("\"") + key + "\"";
    auto pos = body.find(k);
    if (pos == std::string::npos) return 0.0;
    pos = body.find(':', pos);
    if (pos == std::string::npos) return 0.0;
    try { return std::stod(body.substr(pos + 1)); }
    catch (...) { return 0.0; }
}

static int64_t extractInt64(const std::string& body, const char* key)
{
    std::string k = std::string("\"") + key + "\"";
    auto pos = body.find(k);
    if (pos == std::string::npos) return 0;
    pos = body.find(':', pos);
    if (pos == std::string::npos) return 0;
    try { return std::stoll(body.substr(pos + 1)); }
    catch (...) { return 0; }
}

// ---------------------------------------------------------------------------
// GET /api/modules
// Returns JSON array of {id, plugin, slug, name} for every module in the rack.

static void handle_get_modules(const httplib::Request&, httplib::Response& res,
                               CardinalBasePlugin* plugin)
{
    CardinalPluginContext* const context = plugin->context;
    if (!context || !context->engine) {
        res.status = 503;
        res.set_content("{\"error\":\"engine not ready\"}", "application/json");
        return;
    }

    std::string json = "[";
    bool first = true;
    {
        std::lock_guard<std::mutex> mlock(gModuleAccessMutex);
        rack::contextSet(context);
        std::vector<int64_t> ids = context->engine->getModuleIds();
        rack::contextSet(nullptr);
        for (int64_t id : ids) {
            rack::engine::Module* m = context->engine->getModule(id);
            if (!m) continue;
            if (!first) json += ",";
            first = false;
            json += "{\"id\":" + std::to_string(m->id)
                  + ",\"plugin\":\"" + jsonEscape(m->model->plugin->slug) + "\""
                  + ",\"slug\":\"" + jsonEscape(m->model->slug) + "\""
                  + ",\"name\":\"" + jsonEscape(m->model->name) + "\"}";
        }
    }
    json += "]";

    setCorsHeaders(res);
    res.set_content(json, "application/json");
}

// ---------------------------------------------------------------------------
// GET /api/params/:moduleId
// Returns JSON array of {id, name, value, min, max} for every param of that module.

static void handle_get_params(const httplib::Request& req, httplib::Response& res,
                              CardinalBasePlugin* plugin)
{
    const int64_t moduleId = std::stoll(req.path_params.at("moduleId"));

    CardinalPluginContext* const context = plugin->context;
    if (!context || !context->engine) {
        res.status = 503;
        res.set_content("{\"error\":\"engine not ready\"}", "application/json");
        return;
    }

    std::lock_guard<std::mutex> mlock(gModuleAccessMutex);
    rack::engine::Module* const module = context->engine->getModule(moduleId);
    if (!module) {
        res.status = 404;
        res.set_content("{\"error\":\"module not found\"}", "application/json");
        return;
    }

    std::string json = "[";
    bool first = true;
    for (size_t i = 0; i < module->params.size(); ++i) {
        // Use .value directly — getValue() is not const
        float val    = module->params[i].value;
        const rack::ParamQuantity* pq = (module->paramQuantities.size() > i)
                                        ? module->paramQuantities[i] : nullptr;
        // Skip removed/unconfigured params (name == ""): they are placeholder
        // slots from old enum values that were removed in a later version.
        if (!pq || pq->name.empty()) continue;
        if (!first) json += ",";
        first = false;
        std::string name = jsonEscape(pq->name);
        float minVal = pq->minValue;
        float maxVal = pq->maxValue;
        json += "{\"id\":" + std::to_string(i)
              + ",\"name\":\"" + name + "\""
              + ",\"value\":" + floatToJson(val)
              + ",\"min\":" + floatToJson(minVal)
              + ",\"max\":" + floatToJson(maxVal) + "}";
    }
    json += "]";

    setCorsHeaders(res);
    res.set_content(json, "application/json");
}

// ---------------------------------------------------------------------------
// POST /api/param
// Body JSON: {"moduleId": <int64>, "paramId": <int>, "value": <float>}
// Sets an absolute parameter value.

static void handle_post_param(const httplib::Request& req, httplib::Response& res,
                              CardinalBasePlugin* plugin)
{
    CardinalPluginContext* const context = plugin->context;
    if (!context || !context->engine) {
        res.status = 503;
        res.set_content("{\"error\":\"engine not ready\"}", "application/json");
        return;
    }

    const int64_t moduleId = extractInt64(req.body, "moduleId");
    const int     paramId  = static_cast<int>(extractNum(req.body, "paramId"));
    const float   value    = static_cast<float>(extractNum(req.body, "value"));

    std::lock_guard<std::mutex> mlock(gModuleAccessMutex);
    rack::engine::Module* const module = context->engine->getModule(moduleId);
    if (!module) {
        res.status = 404;
        res.set_content("{\"error\":\"module not found\"}", "application/json");
        return;
    }
    if (paramId < 0 || paramId >= static_cast<int>(module->params.size())) {
        res.status = 400;
        res.set_content("{\"error\":\"paramId out of range\"}", "application/json");
        return;
    }

    context->engine->setParamValue(module, paramId, value);

    setCorsHeaders(res);
    res.set_content("{\"ok\":true}", "application/json");
}

// ---------------------------------------------------------------------------
// POST /api/param/delta
// Body JSON: {"moduleId": <int64>, "paramId": <int>, "delta": <float>}
// Adds delta to the current parameter value.
// Correct approach for endless-encoder parameters (range -INF/+INF).

static void handle_post_param_delta(const httplib::Request& req, httplib::Response& res,
                                    CardinalBasePlugin* plugin)
{
    CardinalPluginContext* const context = plugin->context;
    if (!context || !context->engine) {
        res.status = 503;
        res.set_content("{\"error\":\"engine not ready\"}", "application/json");
        return;
    }

    const int64_t moduleId = extractInt64(req.body, "moduleId");
    const int     paramId  = static_cast<int>(extractNum(req.body, "paramId"));
    const float   delta    = static_cast<float>(extractNum(req.body, "delta"));

    std::lock_guard<std::mutex> mlock(gModuleAccessMutex);
    rack::engine::Module* const module = context->engine->getModule(moduleId);
    if (!module) {
        res.status = 404;
        res.set_content("{\"error\":\"module not found\"}", "application/json");
        return;
    }
    if (paramId < 0 || paramId >= static_cast<int>(module->params.size())) {
        res.status = 400;
        res.set_content("{\"error\":\"paramId out of range\"}", "application/json");
        return;
    }

    const float current = module->params[paramId].value;
    const float newVal  = current + delta;
    context->engine->setParamValue(module, paramId, newVal);

    setCorsHeaders(res);
    res.set_content("{\"ok\":true,\"value\":" + std::to_string(newVal) + "}",
                    "application/json");
}

// ---------------------------------------------------------------------------
// GET /api/cables
// Returns JSON array of {id, outModuleId, outPortId, outPortName, outModuleName,
//                           inModuleId,  inPortId,  inPortName,  inModuleName}.

static void handle_get_cables(const httplib::Request&, httplib::Response& res,
                              CardinalBasePlugin* plugin)
{
    CardinalPluginContext* const context = plugin->context;
    if (!context || !context->engine) {
        res.status = 503;
        res.set_content("{\"error\":\"engine not ready\"}", "application/json");
        return;
    }

    // Returns the configured name for a port, empty string if unavailable.
    auto portName = [](rack::engine::Module* mod, int portId, bool output) -> std::string {
        if (!mod) return "";
        auto& infos = output ? mod->outputInfos : mod->inputInfos;
        if (portId < 0 || portId >= (int)infos.size() || !infos[portId]) return "";
        return infos[portId]->getName();
    };

    auto modName = [](rack::engine::Module* mod) -> std::string {
        if (!mod || !mod->model) return "";
        return mod->model->name;
    };

    std::string json = "[";
    bool first = true;
    {
        std::lock_guard<std::mutex> mlock(gModuleAccessMutex);
        rack::contextSet(context);
        std::vector<int64_t> ids = context->engine->getCableIds();
        rack::contextSet(nullptr);
        for (int64_t id : ids) {
            rack::engine::Cable* c = context->engine->getCable(id);
            if (!c) continue;
            if (!first) json += ",";
            first = false;
            json += "{\"id\":" + std::to_string(c->id)
                  + ",\"outModuleId\":" + std::to_string(c->outputModule ? c->outputModule->id : -1LL)
                  + ",\"outPortId\":" + std::to_string(c->outputId)
                  + ",\"outPortName\":\"" + jsonEscape(portName(c->outputModule, c->outputId, true)) + "\""
                  + ",\"outModuleName\":\"" + jsonEscape(modName(c->outputModule)) + "\""
                  + ",\"inModuleId\":" + std::to_string(c->inputModule  ? c->inputModule->id  : -1LL)
                  + ",\"inPortId\":" + std::to_string(c->inputId)
                  + ",\"inPortName\":\"" + jsonEscape(portName(c->inputModule,  c->inputId,  false)) + "\""
                  + ",\"inModuleName\":\"" + jsonEscape(modName(c->inputModule)) + "\"";
            json += "}";
        }
    }
    json += "]";

    setCorsHeaders(res);
    res.set_content(json, "application/json");
}

// ---------------------------------------------------------------------------
// POST /api/cables
// Body: {outModuleId, outPortId, inModuleId, inPortId}
// Creates a cable. Returns {id} of the new cable.

static void handle_post_cables(const httplib::Request& req, httplib::Response& res,
                               CardinalBasePlugin* plugin)
{
    const int64_t outModuleId = extractInt64(req.body, "outModuleId");
    const int64_t outPortId   = (int64_t)extractNum(req.body, "outPortId");
    const int64_t inModuleId  = extractInt64(req.body, "inModuleId");
    const int64_t inPortId    = (int64_t)extractNum(req.body, "inPortId");

    CardinalPluginContext* const context = plugin->context;
    if (!context || !context->engine) {
        res.status = 503;
        res.set_content("{\"error\":\"engine not ready\"}", "application/json");
        return;
    }

    PendingEngineOp op;
    op.type        = PendingEngineOp::ADD_CABLE;
    op.outModuleId = outModuleId;
    op.outPortId   = outPortId;
    op.inModuleId  = inModuleId;
    op.inPortId    = inPortId;

    {
        std::lock_guard<std::mutex> chk(gEngineOpMutex);
        if (gPendingEngineOp) {
            res.status = 429;
            res.set_content("{\"error\":\"engine busy, retry\"}", "application/json");
            return;
        }
        gPendingEngineOp = &op;
    }

    bool completed;
    {
        std::unique_lock<std::mutex> ulock(gEngineOpMutex);
        completed = gEngineOpCV.wait_for(ulock, std::chrono::seconds(3),
            [&]{ return op.done; });
        gPendingEngineOp = nullptr;
    }

    if (!completed) {
        res.status = 504;
        res.set_content("{\"error\":\"timeout: audio not processing\"}", "application/json");
        return;
    }
    if (!op.error.empty()) {
        res.status = (op.error.find("not found") != std::string::npos) ? 404 : 400;
        res.set_content("{\"error\":\"" + op.error + "\"}", "application/json");
        return;
    }
    setCorsHeaders(res);
    res.set_content("{\"id\":" + std::to_string(op.resultId) + "}", "application/json");
}

// ---------------------------------------------------------------------------
// DELETE /api/cables/:cableId
// Removes a cable by ID.

static void handle_delete_cable(const httplib::Request& req, httplib::Response& res,
                                CardinalBasePlugin* plugin)
{
    int64_t cableId;
    try { cableId = std::stoll(req.path_params.at("cableId")); }
    catch (...) {
        res.status = 400;
        res.set_content("{\"error\":\"invalid cableId\"}", "application/json");
        return;
    }

    CardinalPluginContext* const context = plugin->context;
    if (!context || !context->engine) {
        res.status = 503;
        res.set_content("{\"error\":\"engine not ready\"}", "application/json");
        return;
    }

    PendingEngineOp op;
    op.type    = PendingEngineOp::REMOVE_CABLE;
    op.cableId = cableId;

    {
        std::lock_guard<std::mutex> chk(gEngineOpMutex);
        if (gPendingEngineOp) {
            res.status = 429;
            res.set_content("{\"error\":\"engine busy, retry\"}", "application/json");
            return;
        }
        gPendingEngineOp = &op;
    }

    bool completed;
    {
        std::unique_lock<std::mutex> ulock(gEngineOpMutex);
        completed = gEngineOpCV.wait_for(ulock, std::chrono::seconds(3),
            [&]{ return op.done; });
        gPendingEngineOp = nullptr;
    }

    if (!completed) {
        res.status = 504;
        res.set_content("{\"error\":\"timeout: audio not processing\"}", "application/json");
        return;
    }
    if (!op.error.empty()) {
        res.status = (op.error.find("not found") != std::string::npos) ? 404 : 400;
        res.set_content("{\"error\":\"" + op.error + "\"}", "application/json");
        return;
    }
    setCorsHeaders(res);
    res.set_content("{\"ok\":true}", "application/json");
}

// ---------------------------------------------------------------------------
// Minimal JSON string extractor (for plugin/module slugs — no escape handling needed)

static std::string extractString(const std::string& body, const char* key)
{
    std::string k = std::string("\"") + key + "\"";
    auto pos = body.find(k);
    if (pos == std::string::npos) return "";
    pos = body.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = body.find('"', pos);
    if (pos == std::string::npos) return "";
    ++pos;
    auto end = body.find('"', pos);
    if (end == std::string::npos) return "";
    return body.substr(pos, end - pos);
}

// ---------------------------------------------------------------------------
// POST /api/modules
// Body: {pluginSlug, moduleSlug}
// Adds a module to the engine. Returns {id} of the new module.

static void handle_post_modules(const httplib::Request& req, httplib::Response& res,
                                CardinalBasePlugin* plugin)
{
    const std::string pluginSlug = extractString(req.body, "pluginSlug");
    const std::string moduleSlug = extractString(req.body, "moduleSlug");

    if (pluginSlug.empty() || moduleSlug.empty()) {
        res.status = 400;
        res.set_content("{\"error\":\"missing pluginSlug or moduleSlug\"}", "application/json");
        return;
    }

    CardinalPluginContext* const context = plugin->context;
    if (!context || !context->engine) {
        res.status = 503;
        res.set_content("{\"error\":\"engine not ready\"}", "application/json");
        return;
    }

    PendingEngineOp op;
    op.type       = PendingEngineOp::ADD_MODULE;
    op.pluginSlug = pluginSlug;
    op.moduleSlug = moduleSlug;

    {
        std::lock_guard<std::mutex> chk(gEngineOpMutex);
        if (gPendingEngineOp) {
            res.status = 429;
            res.set_content("{\"error\":\"engine busy, retry\"}", "application/json");
            return;
        }
        gPendingEngineOp = &op;
    }

    bool completed;
    {
        std::unique_lock<std::mutex> ulock(gEngineOpMutex);
        completed = gEngineOpCV.wait_for(ulock, std::chrono::seconds(3),
            [&]{ return op.done; });
        gPendingEngineOp = nullptr;
    }

    if (!completed) {
        res.status = 504;
        res.set_content("{\"error\":\"timeout: audio not processing\"}", "application/json");
        return;
    }
    if (!op.error.empty()) {
        res.status = (op.error.find("not found") != std::string::npos) ? 404 : 500;
        res.set_content("{\"error\":\"" + op.error + "\"}", "application/json");
        return;
    }
    setCorsHeaders(res);
    res.set_content("{\"id\":" + std::to_string(op.resultId) + "}", "application/json");
}

// ---------------------------------------------------------------------------
// DELETE /api/modules/:moduleId
// Removes a module and all its connected cables from the engine.

static void handle_delete_module(const httplib::Request& req, httplib::Response& res,
                                 CardinalBasePlugin* plugin)
{
    int64_t moduleId;
    try { moduleId = std::stoll(req.path_params.at("moduleId")); }
    catch (...) {
        res.status = 400;
        res.set_content("{\"error\":\"invalid moduleId\"}", "application/json");
        return;
    }

    CardinalPluginContext* const context = plugin->context;
    if (!context || !context->engine) {
        res.status = 503;
        res.set_content("{\"error\":\"engine not ready\"}", "application/json");
        return;
    }

    PendingEngineOp op;
    op.type     = PendingEngineOp::REMOVE_MODULE;
    op.moduleId = moduleId;

    {
        std::lock_guard<std::mutex> chk(gEngineOpMutex);
        if (gPendingEngineOp) {
            res.status = 429;
            res.set_content("{\"error\":\"engine busy, retry\"}", "application/json");
            return;
        }
        gPendingEngineOp = &op;
    }

    bool completed;
    {
        std::unique_lock<std::mutex> ulock(gEngineOpMutex);
        completed = gEngineOpCV.wait_for(ulock, std::chrono::seconds(3),
            [&]{ return op.done; });
        gPendingEngineOp = nullptr;
    }

    if (!completed) {
        res.status = 504;
        res.set_content("{\"error\":\"timeout: audio not processing\"}", "application/json");
        return;
    }
    if (!op.error.empty()) {
        res.status = (op.error.find("not found") != std::string::npos) ? 404 : 500;
        res.set_content("{\"error\":\"" + op.error + "\"}", "application/json");
        return;
    }
    setCorsHeaders(res);
    res.set_content("{\"ok\":true}", "application/json");
}

// ---------------------------------------------------------------------------
// POST /api/modules/:moduleId/file
// Body: {"key": "wavetable", "path": "/absolute/path/to/file.wav"}
// Calls module->loadFile(key, path). The module handles its own thread safety.
// Designed for file-bearing modules (e.g. Fundamental VCO2, WTLFO wavetables).

static void handle_post_module_file(const httplib::Request& req, httplib::Response& res,
                                    CardinalBasePlugin* plugin)
{
    int64_t moduleId;
    try { moduleId = std::stoll(req.path_params.at("moduleId")); }
    catch (...) {
        res.status = 400;
        res.set_content("{\"error\":\"invalid moduleId\"}", "application/json");
        return;
    }

    // Parse body with jansson so backslashes in Windows paths are unescaped.
    json_error_t jerr;
    json_t* rootJ = json_loadb(req.body.c_str(), req.body.size(), 0, &jerr);
    if (!rootJ) {
        res.status = 400;
        res.set_content("{\"error\":\"invalid JSON body\"}", "application/json");
        return;
    }
    const char* keyC  = json_string_value(json_object_get(rootJ, "key"));
    const char* pathC = json_string_value(json_object_get(rootJ, "path"));
    if (!keyC || !pathC) {
        json_decref(rootJ);
        res.status = 400;
        res.set_content("{\"error\":\"missing key or path\"}", "application/json");
        return;
    }
    std::string key  = keyC;
    std::string path = pathC;
    json_decref(rootJ);

    CardinalPluginContext* const context = plugin->context;
    if (!context || !context->engine) {
        res.status = 503;
        res.set_content("{\"error\":\"engine not ready\"}", "application/json");
        return;
    }

    rack::contextSet(context);
    rack::engine::Module* module = context->engine->getModule(moduleId);
    // Keep context set during loadFile() so APP->engine is valid for modules
    // that call APP->engine->getSampleRate() (e.g. SamplePlayer::updateStepAmount).

    if (!module) {
        rack::contextSet(nullptr);
        res.status = 404;
        res.set_content("{\"error\":\"module not found\"}", "application/json");
        return;
    }

    // loadFile() implementations handle their own thread safety.
    // Modules that use concurrent buffer access (e.g. voxglitch) pre-load into a
    // staging object on this (HTTP) thread and do a fast atomic swap inside process().
    bool handled = module->loadFile(key, path);
    rack::contextSet(nullptr);

    if (!handled) {
        res.status = 400;
        res.set_content("{\"error\":\"module does not support loadFile for key: " + key + "\"}", "application/json");
        return;
    }

    setCorsHeaders(res);
    res.set_content("{\"ok\":true}", "application/json");
}

// ---------------------------------------------------------------------------
// GET /api/patch
// Returns the current patch as JSON text.

static void handle_get_patch(const httplib::Request&, httplib::Response& res,
                             CardinalBasePlugin* plugin)
{
    CardinalPluginContext* const context = plugin->context;
    if (!context || !context->engine) {
        res.status = 503;
        res.set_content("{\"error\":\"engine not ready\"}", "application/json");
        return;
    }

    rack::contextSet(context);
    try {
        json_t* const rootJ = APP->patch->toJson();
        char* const   text  = json_dumps(rootJ, JSON_INDENT(2));
        json_decref(rootJ);
        rack::contextSet(nullptr);
        setCorsHeaders(res);
        res.set_content(text, "application/json");
        std::free(text);
    }
    catch (...) {
        rack::contextSet(nullptr);
        res.status = 500;
        res.set_content("{\"error\":\"failed to serialize patch\"}", "application/json");
    }
}

// ---------------------------------------------------------------------------
// POST /api/patch/load
// Body: raw JSON text of a patch.

static void handle_post_patch_load(const httplib::Request& req, httplib::Response& res,
                                   CardinalBasePlugin* plugin)
{
    CardinalPluginContext* const context = plugin->context;
    if (!context || !context->engine) {
        res.status = 503;
        res.set_content("{\"error\":\"engine not ready\"}", "application/json");
        return;
    }

    // Validate JSON before storing
    json_error_t error;
    json_t* const rootJ = json_loads(req.body.c_str(), 0, &error);
    if (!rootJ) {
        res.status = 400;
        std::string msg = std::string("{\"error\":\"invalid JSON: ")
                        + jsonEscape(error.text) + "\"}";
        res.set_content(msg, "application/json");
        return;
    }
    // If body is {"path": "..."}, delegate the full load() to the UI thread
    // (which calls Manager::load() — handles both V1 and modern .vcv format,
    // calls clear() first to remove existing modules, then extracts + loadAutosave()).
    json_t* const pathJ = json_object_get(rootJ, "path");
    if (pathJ && json_is_string(pathJ)) {
        const std::string vcvPath = json_string_value(pathJ);
        json_decref(rootJ);

        // Quick existence check before queuing.
        {
            FILE* const ftest = std::fopen(vcvPath.c_str(), "rb");
            if (!ftest) {
                res.status = 404;
                setCorsHeaders(res);
                res.set_content("{\"error\":\"cannot open file\"}", "application/json");
                return;
            }
            std::fclose(ftest);
        }

        // Store path and signal the UI thread to call ctx->patch->load(path).
        {
            std::lock_guard<std::mutex> lock(gPendingPathMutex);
            gPendingPatchPath = vcvPath;
        }
        {
            std::lock_guard<std::mutex> lk(gPatchSyncMtx);
            gPatchSyncDone = false;
        }
        gFlagSetCount.fetch_add(1, std::memory_order_relaxed);
        gPendingPatchFromUI.store(true);  // goes directly to UI thread

        // Wait up to 10 s for the UI thread to complete the load.
        bool completed = false;
        {
            std::unique_lock<std::mutex> lk(gPatchSyncMtx);
            completed = gPatchSyncCV.wait_for(lk, std::chrono::seconds(10),
                                              []{ return gPatchSyncDone; });
        }
        setCorsHeaders(res);
        const std::string result = gLastPatchLoadResult;
        res.set_content(completed
            ? ("{\"ok\":true,\"result\":\"" + jsonEscape(result) + "\"}")
            : "{\"ok\":false,\"error\":\"timeout waiting for UI thread\"}",
            "application/json");
        return;
    }
    json_decref(rootJ);

    // Queue JSON-body load: audio thread writes patch.json, UI thread calls loadAutosave().
    // Wait synchronously so the response reflects the actual load result.
    { std::lock_guard<std::mutex> lock(gPendingPathMutex); gPendingPatchPath.clear(); }
    {
        std::lock_guard<std::mutex> lock(gPendingPatchMutex);
        gPendingPatchJson = req.body;
    }
    {
        std::lock_guard<std::mutex> lk(gPatchSyncMtx);
        gPatchSyncDone = false;
    }
    gPendingPatchLoad.store(true);
    gFlagSetCount.fetch_add(1, std::memory_order_relaxed);

    bool completed = false;
    {
        std::unique_lock<std::mutex> lk(gPatchSyncMtx);
        completed = gPatchSyncCV.wait_for(lk, std::chrono::seconds(10),
                                          []{ return gPatchSyncDone; });
    }
    setCorsHeaders(res);
    const std::string result = gLastPatchLoadResult;
    res.set_content(completed
        ? ("{\"ok\":true,\"result\":\"" + jsonEscape(result) + "\"}")
        : "{\"ok\":false,\"error\":\"timeout\"}",
        "application/json");
}

// ---------------------------------------------------------------------------
// Initializer methods

// Called from CardinalPlugin::run() — audio thread, context already set.
// Writes the pending patch JSON to autosavePath and calls loadAutosave(),
// mirroring exactly what CardinalPlugin::setState("patch", ...) does.
void Initializer::processPendingHttpRequests(const std::string& autosavePath,
                                             CardinalPluginContext* ctx)
{
    gRunCallCount.fetch_add(1, std::memory_order_relaxed);

    // Execute any pending engine mutation.
    // Safe here: audio thread is between stepBlock() calls, no engine lock held.
    {
        std::lock_guard<std::mutex> lock(gEngineOpMutex);
        if (gPendingEngineOp && !gPendingEngineOp->done && ctx && ctx->engine) {
            PendingEngineOp* op = gPendingEngineOp;
            switch (op->type) {

            case PendingEngineOp::ADD_MODULE: {
                rack::plugin::Model* model = rack::plugin::getModel(op->pluginSlug, op->moduleSlug);
                if (!model) {
                    op->error = "model not found: " + op->pluginSlug + "/" + op->moduleSlug;
                } else {
                    rack::engine::Module* module = model->createModule();
                    if (!module) {
                        op->error = "createModule failed";
                    } else {
                        ctx->engine->addModule(module);
                        op->resultId = module->id;
                    }
                }
                break;
            }

            case PendingEngineOp::REMOVE_MODULE: {
                rack::engine::Module* module = ctx->engine->getModule(op->moduleId);
                if (!module) {
                    op->error = "module not found";
                } else {
                    std::vector<rack::engine::Cable*> toRemove;
                    for (int64_t cid : ctx->engine->getCableIds()) {
                        rack::engine::Cable* c = ctx->engine->getCable(cid);
                        if (c && (c->inputModule == module || c->outputModule == module))
                            toRemove.push_back(c);
                    }
                    for (rack::engine::Cable* c : toRemove) {
                        ctx->engine->removeCable(c);
                        delete c;
                    }
                    ctx->engine->removeModule(module);
                    delete module;
                }
                break;
            }

            case PendingEngineOp::ADD_CABLE: {
                rack::engine::Module* outMod = ctx->engine->getModule(op->outModuleId);
                rack::engine::Module* inMod  = ctx->engine->getModule(op->inModuleId);
                if (!outMod || !inMod) {
                    op->error = "module not found";
                } else if (op->outPortId < 0 || op->outPortId >= (int64_t)outMod->outputs.size()) {
                    op->error = "invalid outPortId";
                } else if (op->inPortId < 0 || op->inPortId >= (int64_t)inMod->inputs.size()) {
                    op->error = "invalid inPortId";
                } else {
                    rack::engine::Cable* cable = new rack::engine::Cable;
                    cable->outputModule = outMod;
                    cable->outputId     = (int)op->outPortId;
                    cable->inputModule  = inMod;
                    cable->inputId      = (int)op->inPortId;
                    try {
                        ctx->engine->addCable(cable);
                        op->resultId = cable->id;
                    } catch (...) {
                        delete cable;
                        op->error = "addCable failed (port already connected?)";
                    }
                }
                break;
            }

            case PendingEngineOp::REMOVE_CABLE: {
                rack::engine::Cable* cable = ctx->engine->getCable(op->cableId);
                if (!cable) {
                    op->error = "cable not found";
                } else {
                    ctx->engine->removeCable(cable);
                    delete cable;
                }
                break;
            }
            } // switch

            op->done = true;
            gEngineOpCV.notify_all();
        }
    }

    if (!gPendingPatchLoad.load())
        return;

    gFlagDetectedCount.fetch_add(1, std::memory_order_relaxed);

    std::string json;
    {
        std::lock_guard<std::mutex> lock(gPendingPatchMutex);
        json = gPendingPatchJson;
        gPendingPatchJson.clear();
    }
    gPendingPatchLoad.store(false);

    if (!autosavePath.empty())
        rack::system::createDirectories(autosavePath);

    // Inline JSON load: write the body directly as autosave/patch.json.
    const std::string patchFile = rack::system::join(autosavePath, "patch.json");
    FILE* const f = std::fopen(patchFile.c_str(), "w");
    if (!f) {
        gLastPatchLoadResult = "fopen_failed:errno:" + std::to_string(errno);
        d_stdout("Cardinal HTTP: cannot open %s for writing (errno %d)", patchFile.c_str(), errno);
        return;
    }
    std::fwrite(json.c_str(), json.size(), 1, f);
    std::fclose(f);

    // If the UI is not open (APP->scene is null), call loadAutosave() directly here.
    // fromJson() already null-guards all APP->scene accesses, so no GL ops occur —
    // only APP->engine->fromJson() runs, which is safe from the audio thread.
    if (APP->scene == nullptr)
    {
        {
            std::lock_guard<std::mutex> mlock(gModuleAccessMutex);
            try {
                ctx->patch->loadAutosave();
                rack::contextSet(nullptr);
                const size_t modCount = ctx->engine->getModuleIds().size();
                gLastPatchLoadResult = "ok:headless:mods:" + std::to_string(modCount);
                d_stdout("Cardinal HTTP: headless patch loaded OK (%zu modules)", modCount);
            }
            catch (const rack::Exception& e) {
                rack::contextSet(nullptr);
                gLastPatchLoadResult = std::string("rack_ex_headless:") + e.what();
                d_stdout("Cardinal HTTP: headless patch load failed: %s", e.what());
            }
            catch (...) {
                rack::contextSet(nullptr);
                gLastPatchLoadResult = "unknown_ex_headless";
                d_stdout("Cardinal HTTP: headless patch load threw unknown exception");
            }
        }
        { std::lock_guard<std::mutex> lk(gPatchSyncMtx); gPatchSyncDone = true; }
        gPatchSyncCV.notify_all();
        return;
    }

    gLastPatchLoadResult = "file_written_waiting_ui_thread";
    // Signal the UI thread to call loadAutosave() safely.
    gPendingPatchFromUI.store(true);
}

// Called from CardinalUI::uiIdle() — UI thread only.
// For path-based loads: calls ctx->patch->load(path) which handles both V1 and modern
// .vcv format, calls clear() to remove existing modules, then extracts + loadAutosave().
// For JSON-body loads: autosave/patch.json was already written; calls loadAutosave() only.
void httpProcessPendingPatchFromUI(CardinalPluginContext* ctx)
{
    if (!gPendingPatchFromUI.load())
        return;
    gPendingPatchFromUI.store(false);

    // Read path (may be empty for JSON-body loads)
    std::string loadPath;
    {
        std::lock_guard<std::mutex> lock(gPendingPathMutex);
        loadPath = gPendingPatchPath;
        gPendingPatchPath.clear();
    }

    {
        std::lock_guard<std::mutex> mlock(gModuleAccessMutex);
        rack::contextSet(ctx);
        try {
            if (!loadPath.empty()) {
                d_stdout("Cardinal HTTP: UI-thread loading from path: %s", loadPath.c_str());
                ctx->patch->load(loadPath);
            } else {
                ctx->patch->loadAutosave();
            }
            rack::contextSet(nullptr);
            const size_t modCount = ctx->engine->getModuleIds().size();
            gLastPatchLoadResult = "ok:mods:" + std::to_string(modCount);
            d_stdout("Cardinal HTTP: UI-thread patch loaded OK (%zu modules)", modCount);
        }
        catch (const rack::Exception& e) {
            rack::contextSet(nullptr);
            gLastPatchLoadResult = std::string("rack_ex:") + e.what();
            d_stdout("Cardinal HTTP: UI-thread patch load failed: %s", e.what());
        }
        catch (...) {
            rack::contextSet(nullptr);
            gLastPatchLoadResult = "unknown_ex";
            d_stdout("Cardinal HTTP: UI-thread patch load threw unknown exception");
        }
    }

    // Signal any HTTP thread waiting for this load to complete (path-based sync load).
    {
        std::lock_guard<std::mutex> lk(gPatchSyncMtx);
        gPatchSyncDone = true;
    }
    gPatchSyncCV.notify_all();
}


// ---------------------------------------------------------------------------
// WebSocket /api/ws
// Streams live parameter changes every 50 ms.
// JSON frames: [{"m":<moduleId>,"p":<paramId>,"v":<value>}, ...]

static void handle_ws(const httplib::Request&, httplib::ws::WebSocket& ws,
                      CardinalBasePlugin* plugin)
{
    using ParamKey = std::pair<int64_t, int>;
    std::map<ParamKey, float> snapshot;

    while (ws.is_open()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (!plugin) continue;
        CardinalPluginContext* const context = plugin->context;
        if (!context || !context->engine) continue;

        std::vector<int64_t> ids;
        std::map<ParamKey, float> current;
        {
            std::lock_guard<std::mutex> mlock(gModuleAccessMutex);
            ids = context->engine->getModuleIds();
            for (int64_t id : ids) {
                rack::engine::Module* m = context->engine->getModule(id);
                if (!m) continue;
                for (size_t i = 0; i < m->params.size(); ++i)
                    current[{id, (int)i}] = m->params[i].value;
            }
        }

        std::string json = "[";
        bool first = true;
        for (auto& [key, val] : current) {
            auto it = snapshot.find(key);
            if (it == snapshot.end() || it->second != val) {
                if (!first) json += ",";
                first = false;
                json += "{\"m\":" + std::to_string(key.first)
                      + ",\"p\":" + std::to_string(key.second)
                      + ",\"v\":" + floatToJson(val) + "}";
            }
        }
        json += "]";
        snapshot = std::move(current);

        if (json != "[]")
            if (!ws.send(json)) break;
    }
}

void Initializer::startHttpServer()
{
    if (gHttpRunning.load())
        return;

    gHttpServer = new httplib::Server();
    httplib::Server* srv = gHttpServer;

    // CORS pre-flight
    srv->Options(".*", [](const httplib::Request&, httplib::Response& res) {
        setCorsHeaders(res);
        res.status = 204;
    });

    Initializer* const self = this;

    srv->Get("/api/modules", [self](const httplib::Request& req, httplib::Response& res) {
        if (self->httpPluginInstance) handle_get_modules(req, res, self->httpPluginInstance);
        else { res.status = 503; res.set_content("{\"error\":\"no plugin\"}", "application/json"); }
    });

    srv->Get("/api/params/:moduleId", [self](const httplib::Request& req, httplib::Response& res) {
        if (self->httpPluginInstance) handle_get_params(req, res, self->httpPluginInstance);
        else { res.status = 503; res.set_content("{\"error\":\"no plugin\"}", "application/json"); }
    });

    srv->Post("/api/param", [self](const httplib::Request& req, httplib::Response& res) {
        if (self->httpPluginInstance) handle_post_param(req, res, self->httpPluginInstance);
        else { res.status = 503; res.set_content("{\"error\":\"no plugin\"}", "application/json"); }
    });

    srv->Post("/api/param/delta", [self](const httplib::Request& req, httplib::Response& res) {
        if (self->httpPluginInstance) handle_post_param_delta(req, res, self->httpPluginInstance);
        else { res.status = 503; res.set_content("{\"error\":\"no plugin\"}", "application/json"); }
    });

    srv->Get("/api/patch", [self](const httplib::Request& req, httplib::Response& res) {
        if (self->httpPluginInstance) handle_get_patch(req, res, self->httpPluginInstance);
        else { res.status = 503; res.set_content("{\"error\":\"no plugin\"}", "application/json"); }
    });

    srv->Post("/api/patch/load", [self](const httplib::Request& req, httplib::Response& res) {
        if (self->httpPluginInstance) handle_post_patch_load(req, res, self->httpPluginInstance);
        else { res.status = 503; res.set_content("{\"error\":\"no plugin\"}", "application/json"); }
    });

    srv->Get("/api/cables", [self](const httplib::Request& req, httplib::Response& res) {
        if (self->httpPluginInstance) handle_get_cables(req, res, self->httpPluginInstance);
        else { res.status = 503; res.set_content("{\"error\":\"no plugin\"}", "application/json"); }
    });

    srv->Post("/api/cables", [self](const httplib::Request& req, httplib::Response& res) {
        if (self->httpPluginInstance) handle_post_cables(req, res, self->httpPluginInstance);
        else { res.status = 503; res.set_content("{\"error\":\"no plugin\"}", "application/json"); }
    });

    srv->Delete("/api/cables/:cableId", [self](const httplib::Request& req, httplib::Response& res) {
        if (self->httpPluginInstance) handle_delete_cable(req, res, self->httpPluginInstance);
        else { res.status = 503; res.set_content("{\"error\":\"no plugin\"}", "application/json"); }
    });

    srv->Post("/api/modules", [self](const httplib::Request& req, httplib::Response& res) {
        if (self->httpPluginInstance) handle_post_modules(req, res, self->httpPluginInstance);
        else { res.status = 503; res.set_content("{\"error\":\"no plugin\"}", "application/json"); }
    });

    srv->Delete("/api/modules/:moduleId", [self](const httplib::Request& req, httplib::Response& res) {
        if (self->httpPluginInstance) handle_delete_module(req, res, self->httpPluginInstance);
        else { res.status = 503; res.set_content("{\"error\":\"no plugin\"}", "application/json"); }
    });

    srv->Post("/api/modules/:moduleId/file", [self](const httplib::Request& req, httplib::Response& res) {
        if (self->httpPluginInstance) handle_post_module_file(req, res, self->httpPluginInstance);
        else { res.status = 503; res.set_content("{\"error\":\"no plugin\"}", "application/json"); }
    });

    // Audio device & config endpoints (native standalone only; safe to call from all formats)
    srv->Get("/api/audio/devices", [](const httplib::Request&, httplib::Response& res) {
        setCorsHeaders(res);
        res.set_content(gCardinalAudioDevicesJson, "application/json");
    });

    srv->Get("/api/audio/config", [](const httplib::Request&, httplib::Response& res) {
        setCorsHeaders(res);
        res.set_content(gCardinalAudioConfigJson, "application/json");
    });

    srv->Post("/api/audio/config", [](const httplib::Request& req, httplib::Response& res) {
        setCorsHeaders(res);
        const std::string& body = req.body;

        // Simple JSON field extraction (known fixed format)
        auto getStr = [&](const char* key) -> std::string {
            std::string k = std::string("\"") + key + "\"";
            size_t pos = body.find(k);
            if (pos == std::string::npos) return "";
            pos = body.find('"', pos + k.size() + 1);
            if (pos == std::string::npos) return "";
            ++pos;
            size_t end = body.find('"', pos);
            return end == std::string::npos ? "" : body.substr(pos, end - pos);
        };
        auto getInt = [&](const char* key, uint32_t def) -> uint32_t {
            std::string k = std::string("\"") + key + "\"";
            size_t pos = body.find(k);
            if (pos == std::string::npos) return def;
            pos = body.find_first_of("0123456789", pos + k.size());
            if (pos == std::string::npos) return def;
            return (uint32_t)std::stoul(body.substr(pos));
        };

        std::string driver = getStr("driver");
        std::string device = getStr("device");
        uint32_t sampleRate = getInt("sampleRate", 48000);
        uint32_t bufferSize = getInt("bufferSize", 512);

        if (driver.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"missing driver field\"}", "application/json");
            return;
        }

        if (saveAudioConfigFile(driver, device, sampleRate, bufferSize)) {
            res.set_content("{\"ok\":true,\"message\":\"Config saved. Restart Cardinal to apply.\"}", "application/json");
        } else {
            res.status = 500;
            res.set_content("{\"error\":\"failed to save config file\"}", "application/json");
        }
    });

    srv->WebSocket("/api/ws", [self](const httplib::Request& req, httplib::ws::WebSocket& ws) {
        handle_ws(req, ws, self->httpPluginInstance);
    });

    srv->Get("/api/debug", [self](const httplib::Request&, httplib::Response& res) {
        std::string json = "{";
        json += "\"hasPlugin\":" + std::string(self->httpPluginInstance ? "true" : "false");
        if (self->httpPluginInstance) {
            CardinalPluginContext* ctx = self->httpPluginInstance->context;
            json += ",\"hasContext\":" + std::string(ctx ? "true" : "false");
            if (ctx) {
                // Engine module count
                if (ctx->engine) {
                    rack::contextSet(ctx);
                    auto ids = ctx->engine->getModuleIds();
                    json += ",\"engineModuleCount\":" + std::to_string(ids.size());
                    // Scene rack module count (UI side)
                    size_t sceneCount = 0;
                    if (ctx->scene && ctx->scene->rack) {
                        auto widgets = ctx->scene->rack->getModules();
                        sceneCount = widgets.size();
                    }
                    json += ",\"sceneModuleCount\":" + std::to_string(sceneCount);
                    // Engine and context addresses for cross-checking
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%p", static_cast<void*>(ctx));
                    json += ",\"ctxAddr\":\"" + std::string(buf) + "\"";
                    snprintf(buf, sizeof(buf), "%p", static_cast<void*>(ctx->engine));
                    json += ",\"engineAddr\":\"" + std::string(buf) + "\"";
                    json += ",\"autosavePath\":\"" + jsonEscape(ctx->patch->autosavePath) + "\"";
                    json += ",\"sceneNull\":" + std::string(ctx->scene ? "false" : "true");
                    rack::contextSet(nullptr);
                }
            }
        }
        json += ",\"runCallCount\":" + std::to_string(gRunCallCount.load());
        json += ",\"flagSetCount\":" + std::to_string(gFlagSetCount.load());
        json += ",\"flagDetectedCount\":" + std::to_string(gFlagDetectedCount.load());
        json += ",\"pendingNow\":" + std::string(gPendingPatchLoad.load() ? "true" : "false");
        json += ",\"pendingUI\":" + std::string(gPendingPatchFromUI.load() ? "true" : "false");
        json += ",\"lastPatchLoad\":\"" + jsonEscape(gLastPatchLoadResult) + "\"";
        json += ",\"systemDir\":\"" + jsonEscape(rack::asset::systemDir) + "\"";
        json += ",\"bundlePath\":\"" + jsonEscape(rack::asset::bundlePath) + "\"";
        json += ",\"pluginCount\":" + std::to_string(rack::plugin::plugins.size());
        {
            const std::string testPath = rack::system::join(rack::asset::bundlePath, "Fundamental.json");
            json += ",\"fundamentalManifestPath\":\"" + jsonEscape(testPath) + "\"";
            FILE* f1 = std::fopen(testPath.c_str(), "r");
            json += ",\"fundamentalManifestOpen\":" + std::string(f1 ? "true" : "false");
            if (f1) std::fclose(f1);
            // Hard-coded path test (forward slashes only)
            FILE* f2 = std::fopen("C:/Program Files/Common Files/VST3/Postmodular.vst3/Contents/Resources/PluginManifests/Fundamental.json", "r");
            json += ",\"hardcodedPathOpen\":" + std::string(f2 ? "true" : "false");
            if (f2) std::fclose(f2);
            // Check errno
            json += ",\"errno\":" + std::to_string(errno);
        }
        json += "}";
        setCorsHeaders(res);
        res.set_content(json, "application/json");
    });

    srv->Get("/api/plugins", [](const httplib::Request&, httplib::Response& res) {
        std::string json = "[";
        bool first = true;
        for (rack::plugin::Plugin* p : rack::plugin::plugins) {
            if (!first) json += ",";
            first = false;
            json += "{\"slug\":\"" + jsonEscape(p->slug) + "\""
                  + ",\"models\":" + std::to_string(p->models.size()) + "}";
        }
        json += "]";
        setCorsHeaders(res);
        res.set_content(json, "application/json");
    });

    const int port = []() -> int {
        const char* env = std::getenv("CARDINAL_HTTP_PORT");
        return (env && *env) ? std::atoi(env) : kDefaultHttpPort;
    }();

    gHttpRunning.store(true);
    gHttpThread = std::thread([srv, port]() {
        srv->listen("127.0.0.1", port);
    });

    d_stdout("Cardinal HTTP server started on port %d", port);

#if defined(_WIN32) && !defined(DISTRHO_PLUGIN_TARGET_JACK)
    // Auto-launch the accessible companion UI if it is not already open.
    // Only for VST3/CLAP: in standalone mode PostmodularAccessibleUI spawns the engine, not the other way around.
    if (FindWindowW(nullptr, L"Postmodular Accessible") == nullptr)
    {
        const intptr_t result = (intptr_t)ShellExecuteW(
            nullptr, L"open",
            L"C:\\Program Files\\Postmodular\\PostmodularAccessibleUI.exe",
            L"--vst-mode", nullptr, SW_SHOWNORMAL);
        if (result > 32)
            d_stdout("PostmodularAccessibleUI launched");
        else
            d_stdout("Failed to launch PostmodularAccessibleUI (ShellExecuteW=%ld)", (long)result);
    }
#endif
}

void Initializer::stopHttpServer()
{
    if (!gHttpRunning.load())
        return;

    gHttpRunning.store(false);
    if (gHttpServer)
        gHttpServer->stop();
    if (gHttpThread.joinable())
        gHttpThread.join();
    delete gHttpServer;
    gHttpServer = nullptr;
    d_stdout("Cardinal HTTP server stopped");
}

END_NAMESPACE_DISTRHO

#endif // CARDINAL_ACCESSIBLE_HTTP
