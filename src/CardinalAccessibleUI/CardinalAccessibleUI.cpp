/*
 * Cardinal Accessible UI - Strada 2
 *
 * wxWidgets app that starts CardinalNative.exe --hidden and provides a
 * screen-reader-friendly interface with the same features as the Python
 * companion app.
 *
 * Tabs (aligned with Python companion):
 *   Ctrl+1  Parameters  (modules + params inline)
 *   Ctrl+2  Cables
 *   Ctrl+3  Browser     (add modules from catalog)
 *   Ctrl+4  Audio
 *
 * File menu: Ctrl+N new, Ctrl+O open, Ctrl+S save
 * Language menu: built-in English + Italian; community JSON files in lang/
 */

// ── includes ──────────────────────────────────────────────────────────────────

#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/spinctrl.h>
#include <wx/listbox.h>
#include <wx/filedlg.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/file.h>
#include <wx/dir.h>
#include <wx/config.h>
#include <wx/choice.h>
#include <wx/process.h>
#include <wx/utils.h>
#include <wx/textctrl.h>
#include <wx/statline.h>

#ifndef _WIN32_WINNT
# define _WIN32_WINNT 0x0A00
#endif
#include "../extra/httplib.h"

#include "json.hpp"
using json = nlohmann::json;

#include <string>
#include <vector>
#include <map>
#include <set>
#include <fstream>
#include <optional>
#include <cstdlib>
#include <thread>
#include <chrono>

// ── constants ─────────────────────────────────────────────────────────────────

static const char*  HTTP_HOST       = "127.0.0.1";
static const int    HTTP_PORT       = 2229;
static const int    ID_LANG_BASE    = 11000;
static const int    ID_SILENT_REFRESH = 12000;

// ── globals ───────────────────────────────────────────────────────────────────

static wxString g_exedir;  // set in OnInit

// Visual status label (wxStaticText at the bottom of the frame).
// Keeps the last message visible. Set in MainFrame.
static wxStaticText* g_status_label = nullptr;
static bool          g_silent_refresh = false;

// NVDA Controller Client: nvdaController_speakText() sends text directly to
// NVDA's speech queue, bypassing MSAA/UIA entirely. This is the most reliable
// Win32 approach because it does not depend on focus or event routing.
// Loaded dynamically so the app runs even without NVDA installed.
typedef unsigned long (WINAPI *NvdaSpeakText_t)(const wchar_t*);
static NvdaSpeakText_t g_nvda_speak = nullptr;

static void init_nvda_client()
{
    auto get_fn = [](HMODULE h) -> NvdaSpeakText_t {
        return h ? reinterpret_cast<NvdaSpeakText_t>(
            ::GetProcAddress(h, "nvdaController_speakText")) : nullptr;
    };
    // 1. nvdaHelperRemote.dll is injected into every GUI process by NVDA.
    //    Must be called AFTER the first window is created (injection happens then).
    g_nvda_speak = get_fn(::GetModuleHandleW(L"nvdaHelperRemote.dll"));
    if (g_nvda_speak) return;
    // 2. Standalone controller client DLL next to our exe
    std::wstring local = (g_exedir + wxFILE_SEP_PATH +
                          "nvdaControllerClient64.dll").ToStdWstring();
    g_nvda_speak = get_fn(::LoadLibraryW(local.c_str()));
    if (g_nvda_speak) return;
    // 3. Standard NVDA installation path (if separately installed)
    g_nvda_speak = get_fn(
        ::LoadLibraryW(L"C:\\Program Files (x86)\\NVDA\\nvdaControllerClient64.dll"));
}

static void announce(const wxString& msg)
{
    if (g_silent_refresh) return;
    // Lazy init: nvdaHelperRemote.dll is only injected after the first window
    // is created, so GetModuleHandle may fail if called too early.
    if (!g_nvda_speak)
        g_nvda_speak = reinterpret_cast<NvdaSpeakText_t>(
            ::GetProcAddress(::GetModuleHandleW(L"nvdaHelperRemote.dll"),
                             "nvdaController_speakText"));
    if (g_nvda_speak)
        g_nvda_speak(msg.wc_str());
    // Visual: update the label strip and the frame status bar
    if (g_status_label) {
        g_status_label->SetLabel(msg);
        if (auto* f = wxDynamicCast(wxGetTopLevelParent(g_status_label), wxFrame))
            f->SetStatusText(msg);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Translation system
// ══════════════════════════════════════════════════════════════════════════════

// Keys = English text (self-documenting). Values = translation.
static std::map<std::string, std::string> g_translations;
static std::string g_lang_code = "en";

struct LangInfo { std::string code; wxString name; };
static std::vector<LangInfo> g_available_langs;

// Italian built-in strings
static const std::initializer_list<std::pair<const char*, const char*>> kItalianStrings = {
    // ParamsPanel
    {"Modules (Backspace: remove, F5: refresh):", "Moduli (Backspace: rimuovi, F5: aggiorna):"},
    {"Parameters:",                               "Parametri:"},
    {"Parameters of: ",                           "Parametri di: "},
    {"Value:",                                    "Valore:"},
    {"Send (Enter)",                              "Invia (Enter)"},
    {"Reset (Home)",                              "Reset (Home)"},
    {"[encoder]",                                 "[encoder]"},
    {"%d modules loaded.",                        "%d moduli caricati."},
    {"Error: cannot reach Cardinal.",             "Errore: impossibile raggiungere Cardinal."},
    {"Module removal error.",                     "Errore nella rimozione del modulo."},
    {"Module removed.",                           "Modulo rimosso."},
    {"reset to",                                    "ripristinato a"},
    {"delta sent",                                  "delta inviato"},
    {"Trigger (Space)",                             "Trigger (Spazio)"},
    {"triggered",                                   "attivato"},
    {"Load wavetable...",                           "Carica wavetable..."},
    {"Load sample...",                              "Carica campione..."},
    {"Load audio file...",                          "Carica file audio..."},
    {"File load error.",                            "Errore caricamento file."},
    {"Loaded: ",                                    "Caricato: "},
    // CablesPanel
    {"Cables in patch (F5: refresh, Del: remove):", "Cavi nel patch (F5: aggiorna, Del: rimuovi):"},
    {"Refresh (F5)",                              "Aggiorna (F5)"},
    {"Remove cable (Del)",                        "Rimuovi cavo (Del)"},
    {"Cable list",                                "Lista cavi"},
    {"Cable removal error.",                      "Errore nella rimozione del cavo."},
    {"Cable removed.",                            "Cavo rimosso."},
    {"Create new cable:",                         "Crea nuovo cavo:"},
    {"From module:",                              "Modulo sorgente:"},
    {"From output:",                              "Uscita:"},
    {"To module:",                                "Modulo destinazione:"},
    {"To input:",                                 "Ingresso:"},
    {"Create cable (Ctrl+Enter)",                 "Crea cavo (Ctrl+Invio)"},
    {"Select all four fields first.",             "Seleziona tutti e quattro i campi prima."},
    {"Cable created.",                            "Cavo creato."},
    {"Cable creation error.",                     "Errore nella creazione del cavo."},
    // BrowserPanel
    {"Search:",                                   "Cerca:"},
    {"Manufacturer:",                             "Produttore:"},
    {"Type:",                                     "Tipo:"},
    {"Results:",                                  "Risultati:"},
    {"Browser results",                           "Risultati browser"},
    {"Add to patch (Ctrl+Enter)",                 "Aggiungi al patch (Ctrl+Enter)"},
    {"All",                                       "Tutti"},
    {"%d modules found.",                         "%d moduli trovati."},
    {"No module selected.",                       "Nessun modulo selezionato."},
    {"Added: ",                                   "Aggiunto: "},
    {"Module add error.",                         "Errore nell'aggiunta del modulo."},
    {"modules_db.json not found. "
     "Copy it to C:\\Program Files\\Cardinal\\.",
     "modules_db.json non trovato. "
     "Copiarlo in C:\\Program Files\\Cardinal\\."},
    // AudioPanel
    {"Audio settings:",                           "Impostazioni audio:"},
    {"Driver:",                                   "Driver:"},
    {"Device:",                                   "Dispositivo:"},
    {"Sample rate:",                              "Sample rate:"},
    {"Buffer size:",                              "Buffer size:"},
    {"Read current configuration",                "Leggi configurazione attuale"},
    {"Save settings",                             "Salva impostazioni"},
    {"Restart DSP",                               "Riavvia DSP"},
    {"Cardinal not reachable.",                   "Cardinal non raggiungibile."},
    {"Configuration loaded.",                     "Configurazione caricata."},
    {"No device selected.",                       "Nessun dispositivo selezionato."},
    {"Saved.",                                    "Salvato."},
    {"Save error.",                               "Errore nel salvataggio."},
    {"Restarting DSP...",                         "Riavvio DSP in corso..."},
    {"DSP restarted.",                            "DSP riavviato."},
    {"DSP restart failed.",                       "Riavvio DSP fallito."},
    // Tabs
    {"Parameters (Ctrl+1)",                       "Parametri (Ctrl+1)"},
    {"Cables (Ctrl+2)",                           "Cavi (Ctrl+2)"},
    {"Browser (Ctrl+3)",                          "Browser (Ctrl+3)"},
    {"Audio (Ctrl+4)",                            "Audio (Ctrl+4)"},
    // Menus
    {"Options",                                   "Opzioni"},
    {"Silent refresh",                            "Aggiornamento silenzioso"},
    {"File",                                      "File"},
    {"New patch\tCtrl+N",                         "Nuovo patch\tCtrl+N"},
    {"Open patch from file...\tCtrl+O",           "Apri patch da file...\tCtrl+O"},
    {"Save patch to file...\tCtrl+S",             "Salva patch su file...\tCtrl+S"},
    {"Quit",                                      "Esci"},
    {"Language",                                  "Lingua"},
    // Language switch message (shown bilingual intentionally)
    {"language_restart_title",                    "language_restart_title"},
    // Status bar / dialogs
    {"Connecting to Cardinal...",                 "Connessione a Cardinal in corso..."},
    {"Connected to Cardinal.",                    "Connesso a Cardinal."},
    {"Cardinal not responding - retrying...",     "Cardinal non risponde - riprovo..."},
    {"New patch created.",                        "Nuovo patch creato."},
    {"Patch loaded: ",                            "Patch caricata: "},
    {"Patch load error.",                         "Errore nel caricamento della patch."},
    {"Cannot retrieve patch from Cardinal.",      "Impossibile recuperare la patch da Cardinal."},
    {"Cannot open file for writing.",             "Impossibile aprire il file per la scrittura."},
    {"Patch saved: ",                             "Patch salvata: "},
    // File dialogs
    {"Open Cardinal patch",                       "Apri patch Cardinal"},
    {"Save Cardinal patch",                       "Salva patch Cardinal"},
    {"Cardinal patches (*.vcv)|*.vcv|All files (*.*)|*.*",
     "Patch Cardinal (*.vcv)|*.vcv|Tutti i file (*.*)|*.*"},
    {"Cardinal patches (*.vcv)|*.vcv",            "Patch Cardinal (*.vcv)|*.vcv"},
    // Engine startup error
    {"CardinalNative.exe not found.\n\n"
     "Install Cardinal in C:\\Program Files\\Cardinal\\\n"
     "or place CardinalNative.exe in the same folder as this executable.",
     "CardinalNative.exe non trovato.\n\n"
     "Installa Cardinal in C:\\Program Files\\Cardinal\\\n"
     "oppure metti CardinalNative.exe nella stessa cartella di questo eseguibile."},
    {"Cardinal Accessible - Startup Error",       "Cardinal Accessible - Errore avvio"},
};

static wxString tr(const char* key)
{
    auto it = g_translations.find(key);
    if (it != g_translations.end()) return wxString::FromUTF8(it->second);
    return wxString::FromUTF8(key);
}

static void apply_language(const std::string& code)
{
    g_translations.clear();
    g_lang_code = code;

    if (code == "en") return;

    if (code == "it") {
        for (auto& p : kItalianStrings)
            g_translations[p.first] = p.second;
        return;
    }

    // Load from lang/{code}.json — community translations
    wxString path = g_exedir + wxFILE_SEP_PATH + "lang" +
                    wxFILE_SEP_PATH + wxString::FromUTF8(code) + ".json";
    std::ifstream ifs(path.ToStdString());
    if (!ifs.is_open()) return;
    try {
        auto j = json::parse(ifs);
        for (auto& [k, v] : j.items())
            if (!k.empty() && k[0] != '_' && v.is_string())
                g_translations[k] = v.get<std::string>();
    } catch (...) {}
}

static void scan_languages()
{
    g_available_langs.clear();
    g_available_langs.push_back({"en", "English"});
    g_available_langs.push_back({"it", "Italiano"});

    wxString langdir = g_exedir + wxFILE_SEP_PATH + "lang";
    if (!wxDirExists(langdir)) return;

    wxDir dir(langdir);
    wxString file;
    bool ok = dir.GetFirst(&file, "*.json", wxDIR_FILES);
    while (ok) {
        wxString fullpath = langdir + wxFILE_SEP_PATH + file;
        std::ifstream ifs(fullpath.ToStdString());
        if (ifs.is_open()) {
            try {
                auto j = json::parse(ifs);
                std::string code = j.value("_code", "");
                std::string name = j.value("_name", "");
                if (!code.empty() && !name.empty()) {
                    bool dup = false;
                    for (auto& l : g_available_langs)
                        if (l.code == code) { dup = true; break; }
                    if (!dup)
                        g_available_langs.push_back({code, wxString::FromUTF8(name)});
                }
            } catch (...) {}
        }
        ok = dir.GetNext(&file);
    }
}

// ── HTTP client helpers ───────────────────────────────────────────────────────

static httplib::Client& http()
{
    static httplib::Client cli(HTTP_HOST, HTTP_PORT);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);
    return cli;
}

static std::optional<json> http_get_json(const std::string& path)
{
    auto res = http().Get(path.c_str());
    if (!res || res->status != 200) return std::nullopt;
    try { return json::parse(res->body); }
    catch (...) { return std::nullopt; }
}

static std::optional<json> http_post_json(const std::string& path, const json& body)
{
    auto res = http().Post(path.c_str(), body.dump(), "application/json");
    if (!res || res->status != 200) return std::nullopt;
    try { return json::parse(res->body); }
    catch (...) { return std::nullopt; }
}

static bool http_delete(const std::string& path)
{
    auto res = http().Delete(path.c_str());
    return res && res->status == 200;
}

// ── WebSocket client ──────────────────────────────────────────────────────────
// Minimal RFC-6455 text-frame client for the /api/ws live-param stream.
// Runs in a background thread; reconnects automatically every 3 s on disconnect.

class WsClient
{
public:
    ~WsClient() { stop(); }

    void start(std::function<void(const std::string&)> on_msg,
               std::function<void(bool)>               on_status)
    {
        m_running = true;
        m_thread  = std::thread(&WsClient::run, this,
                                std::move(on_msg), std::move(on_status));
    }

    void stop()
    {
        m_running = false;
        close_sock();
        if (m_thread.joinable()) m_thread.join();
    }

    bool connected() const { return m_connected.load(); }

private:
    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::mutex        m_sock_mtx;
    SOCKET            m_sock = INVALID_SOCKET;

    void close_sock()
    {
        std::lock_guard<std::mutex> lk(m_sock_mtx);
        if (m_sock != INVALID_SOCKET) {
            ::shutdown(m_sock, SD_BOTH);
            ::closesocket(m_sock);
            m_sock = INVALID_SOCKET;
        }
    }

    static bool recv_exact(SOCKET s, char* buf, int n)
    {
        for (int got = 0; got < n; ) {
            int r = ::recv(s, buf + got, n - got, 0);
            if (r <= 0) return false;
            got += r;
        }
        return true;
    }

    SOCKET tcp_connect()
    {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return INVALID_SOCKET;
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_port        = htons(HTTP_PORT);
        a.sin_addr.s_addr = ::inet_addr(HTTP_HOST);
        if (::connect(s, (sockaddr*)&a, sizeof a) != 0) {
            ::closesocket(s); return INVALID_SOCKET;
        }
        return s;
    }

    bool ws_handshake(SOCKET s)
    {
        const char* req =
            "GET /api/ws HTTP/1.1\r\n"
            "Host: 127.0.0.1:2229\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        if (::send(s, req, (int)std::strlen(req), 0) < 0) return false;
        std::string resp; resp.reserve(512);
        char c;
        while (resp.size() < 4096) {
            if (::recv(s, &c, 1, 0) <= 0) return false;
            resp += c;
            if (resp.size() >= 4 &&
                resp.compare(resp.size()-4, 4, "\r\n\r\n") == 0) break;
        }
        return resp.find("101") != std::string::npos;
    }

    void recv_loop(SOCKET s, std::function<void(const std::string&)>& on_msg)
    {
        while (m_running) {
            char hdr[2];
            if (!recv_exact(s, hdr, 2)) return;
            int      opcode = (uint8_t)hdr[0] & 0x0F;
            bool     masked = ((uint8_t)hdr[1] & 0x80) != 0;
            uint64_t plen   = (uint8_t)hdr[1] & 0x7F;
            if (plen == 126) {
                char e[2]; if (!recv_exact(s, e, 2)) return;
                plen = ((uint8_t)e[0]<<8)|(uint8_t)e[1];
            } else if (plen == 127) {
                char e[8]; if (!recv_exact(s, e, 8)) return;
                plen = 0;
                for (int i = 0; i < 8; ++i) plen = (plen<<8)|(uint8_t)e[i];
            }
            char mask[4] = {};
            if (masked && !recv_exact(s, mask, 4)) return;
            if (plen > 1u<<20) return;  // cap at 1 MB
            std::string payload((size_t)plen, '\0');
            if (plen && !recv_exact(s, payload.data(), (int)plen)) return;
            if (masked) for (size_t i = 0; i < plen; ++i) payload[i] ^= mask[i%4];
            if (opcode == 0x1) {
                on_msg(payload);
            } else if (opcode == 0x8) {
                return;
            } else if (opcode == 0x9) {
                // pong (opcode 0xA), masked with zero key
                uint8_t pong[6] = {0x8A, 0x80, 0, 0, 0, 0};
                ::send(s, (char*)pong, 6, 0);
            }
        }
    }

    void run(std::function<void(const std::string&)> on_msg,
             std::function<void(bool)>               on_status)
    {
        while (m_running) {
            SOCKET s = tcp_connect();
            if (s != INVALID_SOCKET && ws_handshake(s)) {
                { std::lock_guard<std::mutex> lk(m_sock_mtx); m_sock = s; }
                m_connected = true;
                on_status(true);
                recv_loop(s, on_msg);
                m_connected = false;
                on_status(false);
            } else {
                if (s != INVALID_SOCKET) ::closesocket(s);
            }
            close_sock();
            if (m_running)
                std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }
};

// ── data structures ───────────────────────────────────────────────────────────

struct ModuleInfo {
    int64_t id;
    std::string plugin, model, displayName;
};

struct ParamInfo {
    int id;
    std::string name, type;   // type: "" = continuous, "toggle" = 0/1, "button" = momentary
    double value, min, max, def;
    bool isEndless;
};

struct CableInfo {
    int64_t id, outputModuleId, inputModuleId;
    int outputId, inputId;
    std::string label;
};

struct BrowserEntry {
    std::string plugin, model, name, manufacturer;
    std::vector<std::string> tags;
};

// ── Catalog ───────────────────────────────────────────────────────────────────

struct PortInfo { int id; std::string name; };
struct CatalogParamType { int id; std::string type; };
struct CatalogEntry { std::vector<PortInfo> inputs, outputs; std::vector<CatalogParamType> params; };
static std::map<std::string, std::map<std::string, CatalogEntry>> g_catalog;

static void load_catalog()
{
    wxString p1 = g_exedir + wxFILE_SEP_PATH + "catalog.json";
    wxString p2;
    const char* up = std::getenv("USERPROFILE");
    if (up)
        p2 = wxString::FromUTF8(std::string(up) +
             "\\Documents\\cardinal-accessible-wx\\catalog.json");

    std::string path;
    if      (wxFileExists(p1)) path = p1.ToStdString();
    else if (!p2.empty() && wxFileExists(p2)) path = p2.ToStdString();
    else return;

    std::ifstream ifs(path);
    if (!ifs.is_open()) return;
    try {
        auto j = json::parse(ifs);
        for (auto& [plugin, models] : j.items()) {
            for (auto& [model, entry] : models.items()) {
                CatalogEntry ce;
                if (entry.contains("outputs") && entry["outputs"].is_array())
                    for (auto& p : entry["outputs"])
                        ce.outputs.push_back({p.value("id", 0), p.value("name", "")});
                if (entry.contains("inputs") && entry["inputs"].is_array())
                    for (auto& p : entry["inputs"])
                        ce.inputs.push_back({p.value("id", 0), p.value("name", "")});
                if (entry.contains("params") && entry["params"].is_array())
                    for (auto& p : entry["params"])
                        ce.params.push_back({p.value("id", 0), p.value("type", "")});
                g_catalog[plugin][model] = std::move(ce);
            }
        }
    } catch (...) {}
}

// ── File-loading module registry ──────────────────────────────────────────────
// Mirrors _FILE_LOADING_MODULES in the Python companion app.

struct FileSlot { std::string key, label; };

static std::map<std::pair<std::string,std::string>, std::vector<FileSlot>> build_file_slots()
{
    auto slots_n = [](int n) {
        std::vector<FileSlot> v;
        for (int i = 1; i <= n; ++i)
            v.push_back({"sample_" + std::to_string(i),
                         "Load sample " + std::to_string(i) + "..."});
        return v;
    };
    std::map<std::pair<std::string,std::string>, std::vector<FileSlot>> m;
    m[{"Fundamental", "VCO2"}]                 = {{"wavetable", "Load wavetable..."}};
    m[{"Fundamental", "LFO2"}]                 = {{"wavetable", "Load wavetable..."}};
    m[{"cf",          "PLAY"}]                 = {{"sample",    "Load sample..."}};
    m[{"cf",          "PLAYER"}]               = {{"sample",    "Load sample..."}};
    m[{"Cardinal",    "AudioFile"}]            = {{"sample",    "Load audio file..."}};
    m[{"voxglitch",   "ghosts"}]               = {{"sample",    "Load sample..."}};
    m[{"voxglitch",   "Looper"}]               = {{"sample",    "Load sample..."}};
    m[{"voxglitch",   "repeater"}]             = slots_n(5);
    m[{"voxglitch",   "GrainEngineMK2"}]       = slots_n(5);
    m[{"voxglitch",   "SamplerX8"}]            = slots_n(8);
    m[{"voxglitch",   "Sampler16P"}]           = slots_n(16);
    m[{"SurgeXTRack", "SurgeXTOSCWavetable"}]  = {{"wavetable", "Load wavetable..."}};
    m[{"SurgeXTRack", "SurgeXTOSCWindow"}]     = {{"wavetable", "Load wavetable..."}};
    return m;
}
static const auto kFileSlots = build_file_slots();

static const CatalogEntry* catalog_get(const std::string& plugin, const std::string& model)
{
    auto it1 = g_catalog.find(plugin);
    if (it1 == g_catalog.end()) return nullptr;
    auto it2 = it1->second.find(model);
    if (it2 == it1->second.end()) return nullptr;
    return &it2->second;
}

// ── ParamsPanel (tab 1): module list + parameter editing inline ───────────────

class ParamsPanel : public wxPanel
{
public:
    ParamsPanel(wxWindow* parent) : wxPanel(parent), m_moduleId(-1)
    {
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        sizer->Add(new wxStaticText(this, wxID_ANY,
                       tr("Modules (Backspace: remove, F5: refresh):")),
                   0, wxLEFT | wxTOP, 6);

        m_mod_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                   0, nullptr, wxLB_SINGLE);
        sizer->Add(m_mod_list, 2, wxEXPAND | wxALL, 4);

        m_par_label = new wxStaticText(this, wxID_ANY, tr("Parameters:"));
        sizer->Add(m_par_label, 0, wxLEFT, 6);

        m_par_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                   0, nullptr, wxLB_SINGLE);
        sizer->Add(m_par_list, 3, wxEXPAND | wxALL, 4);

        auto* ctrl_row = new wxBoxSizer(wxHORIZONTAL);
        m_val_label = new wxStaticText(this, wxID_ANY, tr("Value:"));
        ctrl_row->Add(m_val_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        m_spin = new wxSpinCtrlDouble(this, wxID_ANY, "", wxDefaultPosition,
                                      wxSize(120, -1), wxSP_ARROW_KEYS, -1e9, 1e9, 0, 0.01);
        ctrl_row->Add(m_spin, 0, wxRIGHT, 6);
        m_btn_send  = new wxButton(this, wxID_ANY, tr("Send (Enter)"));
        m_btn_reset = new wxButton(this, wxID_ANY, tr("Reset (Home)"));
        ctrl_row->Add(m_btn_send,  0, wxRIGHT, 4);
        ctrl_row->Add(m_btn_reset, 0, wxRIGHT, 8);
        m_btn_trigger = new wxButton(this, wxID_ANY, tr("Trigger (Space)"));
        m_btn_trigger->Hide();
        ctrl_row->Add(m_btn_trigger, 0);
        sizer->Add(ctrl_row, 0, wxALL, 4);

        // File loading buttons — shown only for modules that support loadFile()
        m_file_sep = new wxStaticLine(this);
        m_file_sep->Hide();
        sizer->Add(m_file_sep, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        m_file_panel = new wxPanel(this);
        m_file_sizer = new wxBoxSizer(wxVERTICAL);
        m_file_panel->SetSizer(m_file_sizer);
        m_file_panel->Hide();
        sizer->Add(m_file_panel, 0, wxEXPAND | wxLEFT | wxBOTTOM, 8);

        SetSizer(sizer);

        m_mod_list->Bind(wxEVT_LISTBOX,  [this](wxCommandEvent&) { on_module_selected(); });
        m_mod_list->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
            switch (e.GetKeyCode()) {
            case WXK_BACK:
            case WXK_DELETE: remove_selected_module(); break;
            case WXK_F5:     refresh(); break;
            default:         e.Skip();
            }
        });
        m_par_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { on_param_selected(); });
        m_btn_send   ->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send_value(); });
        m_btn_reset  ->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { reset_value(); });
        m_btn_trigger->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { trigger_param(); });

        // Bind directly to the spin's internal wxTextCtrl.
        // wxSpinCtrlDouble on Windows is a composite Win32 control: the keyboard
        // events land on the internal EDIT HWND, not on the outer wxSpinCtrlDouble.
        // GetTextCtrl() returns the wxWindow wrapper for that EDIT, so binding
        // wxEVT_KEY_DOWN there is the only reliable interception point.
        if (auto* tc = m_spin->GetText()) {
            tc->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
                int key = e.GetKeyCode();
                if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
                    send_value();
                    return;
                }
                if (key == WXK_HOME) {
                    reset_value();
                    return;
                }
                // Page Up/Down: explicit large step = (max-min)/100 (10× arrow step).
                // Handled manually to guarantee consistent behaviour across wx versions.
                if (key == WXK_PAGEUP || key == WXK_PAGEDOWN) {
                    int sel = m_paramIdx;
                    if (sel >= 0 && sel < (int)m_params.size()) {
                        const ParamInfo& p = m_params[sel];
                        double step = p.isEndless ? 10.0 : (p.max - p.min) / 100.0;
                        double nv = m_spin->GetValue() + (key == WXK_PAGEUP ? step : -step);
                        if (!p.isEndless) {
                            if (nv < p.min) nv = p.min;
                            if (nv > p.max) nv = p.max;
                        }
                        m_spin->SetValue(nv);
                        send_value();
                    }
                    return;
                }
                // Arrows: let the spin update its value, then send.
                if (key == WXK_UP || key == WXK_DOWN ||
                    key == WXK_NUMPAD_UP || key == WXK_NUMPAD_DOWN) {
                    e.Skip();
                    wxTheApp->CallAfter([this]() { send_value(); });
                    return;
                }
                e.Skip();
            });
        }

        // Alt+1 / Alt+2: focus module list / param list (same as Python companion)
        // Space: fire trigger when a button param is selected
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
            if (e.AltDown()) {
                if (e.GetKeyCode() == '1') { m_mod_list->SetFocus(); return; }
                if (e.GetKeyCode() == '2') { m_par_list->SetFocus(); return; }
            }
            if (e.GetKeyCode() == WXK_SPACE && m_btn_trigger->IsShown()) {
                trigger_param();
                return;
            }
            e.Skip();
        });
    }

    void refresh()
    {
        auto result = http_get_json("/api/modules");
        if (!result) {
            set_status(tr("Error: cannot reach Cardinal."));
            return;
        }
        m_modules.clear();
        m_par_list->Clear();
        m_par_label->SetLabel(tr("Parameters:"));
        m_moduleId  = -1;
        m_paramIdx  = -1;
        update_file_buttons("", "");

        for (auto& m : *result) {
            ModuleInfo mi;
            mi.id          = m.value("id", int64_t(0));
            mi.plugin      = m.value("plugin", "");
            mi.model       = m.value("slug", "");        // server sends "slug", not "model"
            mi.displayName = m.value("name", mi.model);  // server sends "name", not "displayName"
            m_modules.push_back(mi);
        }

        m_mod_list->Freeze();
        m_mod_list->Clear();
        for (auto& mi : m_modules)
            m_mod_list->Append(wxString::FromUTF8(mi.displayName + " (" + mi.plugin + ")"));
        m_mod_list->Thaw();

        if (!g_silent_refresh)
            set_status(wxString::Format(tr("%d modules loaded."), (int)m_modules.size()));
    }

    // Called from the main thread by WS live-param updates.
    // Updates the label in-place (preserves NVDA focus) without touching the spin.
    void update_param(int64_t moduleId, int paramId, double value)
    {
        if (moduleId != m_moduleId) return;
        for (int i = 0; i < (int)m_params.size(); ++i) {
            if (m_params[i].id == paramId && !m_params[i].isEndless) {
                m_params[i].value = value;
                m_par_list->SetString(i,
                    wxString::FromUTF8(m_params[i].name) +
                    wxString::Format(" [%.3g]", value));
                break;
            }
        }
    }

private:
    wxListBox*        m_mod_list;
    wxStaticText*     m_par_label;
    wxListBox*        m_par_list;
    wxStaticText*     m_val_label;
    wxSpinCtrlDouble* m_spin;
    wxButton*         m_btn_send;
    wxButton*         m_btn_reset;
    wxButton*         m_btn_trigger;
    wxStaticLine*     m_file_sep;
    wxPanel*          m_file_panel;
    wxBoxSizer*       m_file_sizer;
    std::vector<ModuleInfo> m_modules;
    std::vector<ParamInfo>  m_params;
    int64_t m_moduleId;
    int     m_paramIdx = -1;  // persists selected param index even when listbox loses focus

    void set_status(const wxString& s)
    {
        if (auto* f = wxDynamicCast(wxGetTopLevelParent(this), wxFrame))
            f->SetStatusText(s);
    }

    void on_module_selected()
    {
        int sel = m_mod_list->GetSelection();
        if (sel < 0 || sel >= (int)m_modules.size()) return;
        m_moduleId = m_modules[sel].id;
        m_par_label->SetLabel(tr("Parameters:") + " " +
            wxString::FromUTF8(m_modules[sel].displayName));
        load_params();
        update_file_buttons(m_modules[sel].plugin, m_modules[sel].model);
    }

    void load_params()
    {
        m_params.clear();
        m_par_list->Clear();
        m_paramIdx = -1;
        if (m_moduleId < 0) return;
        auto result = http_get_json("/api/params/" + std::to_string(m_moduleId));
        if (!result) return;

        // Find current module for catalog type lookup
        const ModuleInfo* cur_mod = nullptr;
        for (auto& m : m_modules)
            if (m.id == m_moduleId) { cur_mod = &m; break; }
        const CatalogEntry* ce = cur_mod ? catalog_get(cur_mod->plugin, cur_mod->model) : nullptr;

        for (auto& p : *result) {
            ParamInfo pi;
            pi.id       = p.value("id", 0);
            pi.name     = p.value("name", "");
            pi.def      = p.value("default", 0.0);
            pi.isEndless = p["value"].is_null() || p["min"].is_null();
            pi.value = pi.isEndless ? 0.0  : p["value"].get<double>();
            pi.min   = pi.isEndless ? -1e9 : p["min"].get<double>();
            pi.max   = pi.isEndless ?  1e9 : p["max"].get<double>();
            pi.type  = "";
            if (ce)
                for (auto& cp : ce->params)
                    if (cp.id == pi.id) { pi.type = cp.type; break; }
            m_params.push_back(pi);

            wxString lbl = wxString::FromUTF8(pi.name);
            lbl += pi.isEndless ? " " + tr("[encoder]") :
                                  wxString::Format(" [%.3g]", pi.value);
            m_par_list->Append(lbl);
        }
        if (!m_params.empty()) {
            m_par_list->SetSelection(0);
            on_param_selected();
        }
    }

    void on_param_selected()
    {
        int sel = m_par_list->GetSelection();
        if (sel < 0 || sel >= (int)m_params.size()) return;
        m_paramIdx = sel;  // persist so send_value() works even after listbox loses focus
        const ParamInfo& p = m_params[sel];

        bool is_btn = (p.type == "button");
        m_val_label  ->Show(!is_btn);
        m_spin       ->Show(!is_btn);
        m_btn_send   ->Show(!is_btn);
        m_btn_reset  ->Show(!is_btn);
        m_btn_trigger->Show(is_btn);
        Layout();

        if (!is_btn) {
            m_spin->SetRange(p.min, p.max);
            m_spin->SetValue(p.value);
            double inc = p.isEndless   ? 1.0 :
                         p.type == "toggle" ? 1.0 :
                         (p.max - p.min) / 1000.0;
            m_spin->SetIncrement(inc);
        }
    }

    void send_value()
    {
        int sel = m_paramIdx;  // use persistent index — listbox may deselect when spin has focus
        if (sel < 0 || sel >= (int)m_params.size() || m_moduleId < 0) return;
        const ParamInfo& p = m_params[sel];
        double v = m_spin->GetValue();
        if (p.isEndless) {
            if (http_post_json("/api/param/delta",
                    {{"moduleId", m_moduleId}, {"paramId", p.id}, {"delta", v}})) {
                wxString sign = v >= 0 ? "+" : "";
                announce(wxString::FromUTF8(p.name) + ": " + sign +
                         wxString::Format("%.4g ", v) + tr("delta sent"));
            }
        } else {
            if (http_post_json("/api/param",
                    {{"moduleId", m_moduleId}, {"paramId", p.id}, {"value", v}})) {
                m_params[sel].value = v;
                m_par_list->SetString(sel, wxString::FromUTF8(p.name) +
                    wxString::Format(" [%.3g]", v));
                announce(wxString::FromUTF8(p.name) + wxString::Format(" = %.4g", v));
            }
        }
    }

    void reset_value()
    {
        int sel = m_paramIdx;  // use persistent index — listbox may deselect when spin has focus
        if (sel < 0 || sel >= (int)m_params.size() || m_moduleId < 0) return;
        const ParamInfo& p = m_params[sel];
        if (http_post_json("/api/param",
                {{"moduleId", m_moduleId}, {"paramId", p.id}, {"value", p.def}})) {
            m_params[sel].value = p.def;
            m_spin->SetValue(p.def);
            m_par_list->SetString(sel, wxString::FromUTF8(p.name) +
                wxString::Format(" [%.3g]", p.def));
            announce(wxString::FromUTF8(p.name) + " " + tr("reset to") +
                     wxString::Format(" %.4g", p.def));
        }
    }

    void update_file_buttons(const std::string& plugin, const std::string& model)
    {
        m_file_sizer->Clear(true);  // destroy existing buttons

        auto it = kFileSlots.find({plugin, model});
        if (it == kFileSlots.end()) {
            m_file_panel->Hide();
            m_file_sep->Hide();
            Layout();
            return;
        }

        for (const auto& slot : it->second) {
            wxString lbl = tr(slot.label.c_str());
            auto* btn = new wxButton(m_file_panel, wxID_ANY, lbl);
            std::string key = slot.key;
            btn->Bind(wxEVT_BUTTON, [this, key](wxCommandEvent&) {
                load_module_file(key);
            });
            m_file_sizer->Add(btn, 0, wxBOTTOM, 4);
        }

        m_file_panel->Layout();
        m_file_sep->Show();
        m_file_panel->Show();
        Layout();
    }

    void load_module_file(const std::string& file_key)
    {
        if (m_moduleId < 0) return;

        wxString wildcard = (file_key == "wavetable")
            ? "Wavetable files (*.wav;*.wt)|*.wav;*.wt|All files (*.*)|*.*"
            : "Audio files (*.wav;*.flac;*.ogg;*.mp3;*.aif;*.aiff)"
              "|*.wav;*.flac;*.ogg;*.mp3;*.aif;*.aiff|All files (*.*)|*.*";

        wxFileDialog dlg(this, "Load file", "", "", wildcard,
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_CANCEL) return;

        wxString path = dlg.GetPath();
        auto res = http_post_json(
            "/api/modules/" + std::to_string(m_moduleId) + "/file",
            {{"key", file_key}, {"path", path.utf8_string()}});

        if (res) {
            std::string modname;
            for (auto& m : m_modules)
                if (m.id == m_moduleId) { modname = m.displayName; break; }
            announce(tr("Loaded: ") + wxFileName(path).GetFullName() +
                     " → " + wxString::FromUTF8(modname));
        } else {
            announce(tr("File load error."));
        }
    }

    void trigger_param()
    {
        int sel = m_paramIdx;
        if (sel < 0 || sel >= (int)m_params.size() || m_moduleId < 0) return;
        const ParamInfo& p = m_params[sel];
        if (http_post_json("/api/param",
                {{"moduleId", m_moduleId}, {"paramId", p.id}, {"value", 1.0}})) {
            announce(wxString::FromUTF8(p.name) + " " + tr("triggered"));
            // Release the button after 100ms on the main thread
            int64_t mid = m_moduleId;
            int     pid = p.id;
            std::thread([mid, pid]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                wxTheApp->CallAfter([mid, pid]() {
                    http_post_json("/api/param",
                        {{"moduleId", mid}, {"paramId", pid}, {"value", 0.0}});
                });
            }).detach();
        }
    }

    void remove_selected_module()
    {
        int sel = m_mod_list->GetSelection();
        if (sel < 0 || sel >= (int)m_modules.size()) return;
        if (http_delete("/api/modules/" + std::to_string(m_modules[sel].id))) {
            announce(tr("Module removed."));
            refresh();
        } else {
            announce(tr("Module removal error."));
        }
    }
};

// ── CablesPanel (tab 2) ───────────────────────────────────────────────────────

class CablesPanel : public wxPanel
{
public:
    CablesPanel(wxWindow* parent) : wxPanel(parent)
    {
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        // ── Section 1: existing cables ──────────────────────────────────────
        sizer->Add(new wxStaticText(this, wxID_ANY,
                       tr("Cables in patch (F5: refresh, Del: remove):")),
                   0, wxALL, 6);

        m_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               0, nullptr, wxLB_SINGLE);
        m_list->SetName(tr("Cable list"));
        sizer->Add(m_list, 1, wxEXPAND | wxALL, 4);

        auto* btn_row = new wxBoxSizer(wxHORIZONTAL);
        auto* btn_refresh = new wxButton(this, wxID_ANY, tr("Refresh (F5)"));
        m_btn_delete = new wxButton(this, wxID_ANY, tr("Remove cable (Del)"));
        m_btn_delete->Disable();
        btn_row->Add(btn_refresh,  0, wxRIGHT, 6);
        btn_row->Add(m_btn_delete, 0);
        sizer->Add(btn_row, 0, wxALL, 4);

        // ── Separator ───────────────────────────────────────────────────────
        sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        // ── Section 2: create new cable ─────────────────────────────────────
        sizer->Add(new wxStaticText(this, wxID_ANY, tr("Create new cable:")),
                   0, wxLEFT | wxTOP, 8);

        auto* grid = new wxFlexGridSizer(4, 2, 6, 8);
        grid->AddGrowableCol(1);

        grid->Add(new wxStaticText(this, wxID_ANY, tr("From module:")),
                  0, wxALIGN_CENTER_VERTICAL);
        m_from_mod = new wxChoice(this, wxID_ANY);
        grid->Add(m_from_mod, 1, wxEXPAND);

        grid->Add(new wxStaticText(this, wxID_ANY, tr("From output:")),
                  0, wxALIGN_CENTER_VERTICAL);
        m_from_out = new wxChoice(this, wxID_ANY);
        grid->Add(m_from_out, 1, wxEXPAND);

        grid->Add(new wxStaticText(this, wxID_ANY, tr("To module:")),
                  0, wxALIGN_CENTER_VERTICAL);
        m_to_mod = new wxChoice(this, wxID_ANY);
        grid->Add(m_to_mod, 1, wxEXPAND);

        grid->Add(new wxStaticText(this, wxID_ANY, tr("To input:")),
                  0, wxALIGN_CENTER_VERTICAL);
        m_to_in = new wxChoice(this, wxID_ANY);
        grid->Add(m_to_in, 1, wxEXPAND);

        sizer->Add(grid, 0, wxEXPAND | wxALL, 8);

        m_btn_create = new wxButton(this, wxID_ANY, tr("Create cable (Ctrl+Enter)"));
        sizer->Add(m_btn_create, 0, wxLEFT | wxBOTTOM, 8);

        m_status = new wxStaticText(this, wxID_ANY, "");
        sizer->Add(m_status, 0, wxALL, 4);

        SetSizer(sizer);

        // ── Bindings ─────────────────────────────────────────────────────────
        btn_refresh ->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh(); });
        m_btn_delete->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { remove_selected(); });
        m_btn_create->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { create_cable(); });

        m_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) {
            m_btn_delete->Enable(m_list->GetSelection() >= 0);
        });
        m_list->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
            int k = e.GetKeyCode();
            if      (k == WXK_DELETE || k == WXK_BACK) remove_selected();
            else if (k == WXK_F5)                      refresh();
            else                                       e.Skip();
        });

        m_from_mod->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { on_from_mod_changed(); });
        m_to_mod  ->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { on_to_mod_changed(); });

        // Alt+1 = cable list, Alt+2 = From module; Ctrl+Enter on To input = create
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
            if (e.AltDown()) {
                if (e.GetKeyCode() == '1') { m_list->SetFocus(); return; }
                if (e.GetKeyCode() == '2') { m_from_mod->SetFocus(); return; }
            }
            if (e.ControlDown() &&
                (e.GetKeyCode() == WXK_RETURN || e.GetKeyCode() == WXK_NUMPAD_ENTER) &&
                wxWindow::FindFocus() == m_to_in)
            {
                create_cable();
                return;
            }
            e.Skip();
        });
    }

    void refresh()
    {
        populate_modules();
        reload_cables();
    }

private:
    wxListBox*    m_list;
    wxButton*     m_btn_delete;
    wxChoice*     m_from_mod;
    wxChoice*     m_from_out;
    wxChoice*     m_to_mod;
    wxChoice*     m_to_in;
    wxButton*     m_btn_create;
    wxStaticText* m_status;

    std::vector<CableInfo>  m_cables;
    std::vector<ModuleInfo> m_modules;

    void set_status(const wxString& s)
    {
        if (auto* f = wxDynamicCast(wxGetTopLevelParent(this), wxFrame))
            f->SetStatusText(s);
    }

    const ModuleInfo* find_module(int64_t id) const
    {
        for (auto& m : m_modules)
            if (m.id == id) return &m;
        return nullptr;
    }

    std::string get_module_name(int64_t id) const
    {
        const auto* m = find_module(id);
        return m ? m->displayName : "mod" + std::to_string(id);
    }

    std::string get_port_name(int64_t module_id, int port_id, bool output) const
    {
        const auto* m = find_module(module_id);
        if (m) {
            const auto* ce = catalog_get(m->plugin, m->model);
            if (ce) {
                const auto& ports = output ? ce->outputs : ce->inputs;
                for (auto& p : ports)
                    if (p.id == port_id)
                        return p.name.empty() ?
                            (output ? "out " : "in ") + std::to_string(port_id) : p.name;
            }
        }
        return (output ? "out " : "in ") + std::to_string(port_id);
    }

    int get_port_id_from_list(const ModuleInfo& m, bool output, int list_idx) const
    {
        const auto* ce = catalog_get(m.plugin, m.model);
        if (ce) {
            const auto& ports = output ? ce->outputs : ce->inputs;
            if (list_idx >= 0 && list_idx < (int)ports.size())
                return ports[list_idx].id;
        }
        return list_idx;  // fallback: index == id
    }

    void populate_ports(wxChoice* choice, int64_t module_id, bool output)
    {
        choice->Freeze();
        choice->Clear();
        const auto* m = find_module(module_id);
        if (m) {
            const auto* ce = catalog_get(m->plugin, m->model);
            if (ce) {
                const auto& ports = output ? ce->outputs : ce->inputs;
                for (auto& p : ports) {
                    std::string lbl = p.name.empty() ?
                        (output ? "Output " : "Input ") + std::to_string(p.id) : p.name;
                    choice->Append(wxString::FromUTF8(lbl));
                }
            } else {
                for (int i = 0; i < 16; ++i)
                    choice->Append(wxString::FromUTF8(
                        (output ? "Output " : "Input ") + std::to_string(i)));
            }
        }
        if (choice->GetCount() > 0) choice->SetSelection(0);
        choice->Thaw();
    }

    void on_from_mod_changed()
    {
        int sel = m_from_mod->GetSelection();
        if (sel < 0 || sel >= (int)m_modules.size()) { m_from_out->Clear(); return; }
        populate_ports(m_from_out, m_modules[sel].id, true);
    }

    void on_to_mod_changed()
    {
        int sel = m_to_mod->GetSelection();
        if (sel < 0 || sel >= (int)m_modules.size()) { m_to_in->Clear(); return; }
        populate_ports(m_to_in, m_modules[sel].id, false);
    }

    void populate_modules()
    {
        auto result = http_get_json("/api/modules");
        if (!result) return;

        // Save current selections to restore after repopulation
        int from_sel = m_from_mod->GetSelection();
        int to_sel   = m_to_mod->GetSelection();
        int64_t from_id = (from_sel >= 0 && from_sel < (int)m_modules.size()) ?
                          m_modules[from_sel].id : -1;
        int64_t to_id   = (to_sel   >= 0 && to_sel   < (int)m_modules.size()) ?
                          m_modules[to_sel].id : -1;

        m_modules.clear();
        for (auto& m : *result) {
            ModuleInfo mi;
            mi.id          = m.value("id", int64_t(0));
            mi.plugin      = m.value("plugin", "");
            mi.model       = m.value("slug", "");
            mi.displayName = m.value("name", mi.model);
            m_modules.push_back(mi);
        }
        std::sort(m_modules.begin(), m_modules.end(),
                  [](const ModuleInfo& a, const ModuleInfo& b) {
                      return a.displayName < b.displayName;
                  });

        int new_from = 0, new_to = 0;
        for (int i = 0; i < (int)m_modules.size(); ++i) {
            if (m_modules[i].id == from_id) new_from = i;
            if (m_modules[i].id == to_id)   new_to   = i;
        }

        auto fill_ch = [&](wxChoice* ch, int sel) {
            ch->Freeze();
            ch->Clear();
            for (auto& mi : m_modules)
                ch->Append(wxString::FromUTF8(mi.displayName + " (" + mi.plugin + ")"));
            if (!m_modules.empty()) ch->SetSelection(sel);
            ch->Thaw();
        };

        fill_ch(m_from_mod, new_from);
        on_from_mod_changed();

        fill_ch(m_to_mod, new_to);
        on_to_mod_changed();
    }

    void reload_cables()
    {
        m_cables.clear();
        m_list->Clear();
        m_btn_delete->Disable();
        auto result = http_get_json("/api/cables");
        if (!result) return;

        for (auto& c : *result) {
            CableInfo ci;
            ci.id             = c.value("id",          int64_t(0));
            ci.outputModuleId = c.value("outModuleId", int64_t(0));
            ci.inputModuleId  = c.value("inModuleId",  int64_t(0));
            ci.outputId       = c.value("outPortId",   0);
            ci.inputId        = c.value("inPortId",    0);

            // Prefer names from server (live); fall back to catalog.json lookup.
            std::string out_name = c.value("outModuleName", "");
            if (out_name.empty()) out_name = get_module_name(ci.outputModuleId);
            std::string in_name  = c.value("inModuleName",  "");
            if (in_name.empty())  in_name  = get_module_name(ci.inputModuleId);
            std::string out_port = c.value("outPortName",   "");
            if (out_port.empty()) out_port = get_port_name(ci.outputModuleId, ci.outputId, true);
            std::string in_port  = c.value("inPortName",    "");
            if (in_port.empty())  in_port  = get_port_name(ci.inputModuleId,  ci.inputId,  false);
            ci.label = out_name + "  " + out_port + "  ->  " + in_name + "  " + in_port;

            m_cables.push_back(ci);
            m_list->Append(wxString::FromUTF8(ci.label));
        }
    }

    void create_cable()
    {
        int from_mod_sel = m_from_mod->GetSelection();
        int from_out_sel = m_from_out->GetSelection();
        int to_mod_sel   = m_to_mod->GetSelection();
        int to_in_sel    = m_to_in->GetSelection();

        if (from_mod_sel < 0 || from_mod_sel >= (int)m_modules.size() ||
            to_mod_sel   < 0 || to_mod_sel   >= (int)m_modules.size() ||
            from_out_sel < 0 || to_in_sel    < 0) {
            m_status->SetLabel(tr("Select all four fields first."));
            return;
        }

        const auto& from_m = m_modules[from_mod_sel];
        const auto& to_m   = m_modules[to_mod_sel];

        int out_id = get_port_id_from_list(from_m, true,  from_out_sel);
        int in_id  = get_port_id_from_list(to_m,   false, to_in_sel);

        // Field names must match the C++ server's extractInt64 keys
        json body = {
            {"outModuleId", from_m.id},
            {"outPortId",   out_id},
            {"inModuleId",  to_m.id},
            {"inPortId",    in_id}
        };

        auto res = http().Post("/api/cables", body.dump(), "application/json");
        if (res && res->status == 200) {
            announce(tr("Cable created."));
            reload_cables();
        } else {
            std::string err = "Cable creation error.";
            if (!res) {
                err += " (no response)";
            } else {
                err += " (HTTP " + std::to_string(res->status) + ")";
                try {
                    auto j = json::parse(res->body);
                    if (j.contains("error"))
                        err += ": " + j["error"].get<std::string>();
                } catch (...) {}
            }
            announce(wxString::FromUTF8(err));
        }
    }

    void remove_selected()
    {
        int sel = m_list->GetSelection();
        if (sel < 0 || sel >= (int)m_cables.size()) return;
        if (http_delete("/api/cables/" + std::to_string(m_cables[sel].id))) {
            announce(tr("Cable removed."));
            reload_cables();
        } else {
            announce(tr("Cable removal error."));
        }
    }
};

// ── BrowserPanel (tab 3) ──────────────────────────────────────────────────────

class BrowserPanel : public wxPanel
{
public:
    std::function<void()> onModuleAdded;  // called after successful POST /api/modules

    BrowserPanel(wxWindow* parent) : wxPanel(parent)
    {
        load_db();

        auto* sizer = new wxBoxSizer(wxVERTICAL);

        auto* grid = new wxFlexGridSizer(3, 2, 6, 8);
        grid->AddGrowableCol(1);

        grid->Add(new wxStaticText(this, wxID_ANY, tr("Search:")),
                  0, wxALIGN_CENTER_VERTICAL);
        m_search = new wxTextCtrl(this, wxID_ANY);
        m_search->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { refresh_results(); });
        grid->Add(m_search, 1, wxEXPAND);

        grid->Add(new wxStaticText(this, wxID_ANY, tr("Manufacturer:")),
                  0, wxALIGN_CENTER_VERTICAL);
        m_mfr = new wxChoice(this, wxID_ANY);
        m_mfr->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refresh_results(); });
        grid->Add(m_mfr, 1, wxEXPAND);

        grid->Add(new wxStaticText(this, wxID_ANY, tr("Type:")),
                  0, wxALIGN_CENTER_VERTICAL);
        m_tag = new wxChoice(this, wxID_ANY);
        m_tag->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refresh_results(); });
        grid->Add(m_tag, 1, wxEXPAND);

        sizer->Add(grid, 0, wxEXPAND | wxALL, 8);

        sizer->Add(new wxStaticText(this, wxID_ANY, tr("Results:")), 0, wxLEFT, 8);
        m_results = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  0, nullptr, wxLB_SINGLE);
        m_results->SetName(tr("Browser results"));
        sizer->Add(m_results, 1, wxEXPAND | wxALL, 8);

        m_btn_add = new wxButton(this, wxID_ANY, tr("Add to patch (Ctrl+Enter)"));
        m_btn_add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { add_module(); });
        sizer->Add(m_btn_add, 0, wxLEFT | wxBOTTOM, 8);

        m_status = new wxStaticText(this, wxID_ANY, "");
        sizer->Add(m_status, 0, wxALL, 6);

        SetSizer(sizer);

        // wxEVT_CHAR_HOOK on the panel catches Ctrl+Enter reliably regardless of
        // which child has focus (wxEVT_KEY_DOWN on wxListBox misses Return on Windows)
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
            if (e.ControlDown() &&
                (e.GetKeyCode() == WXK_RETURN || e.GetKeyCode() == WXK_NUMPAD_ENTER))
                add_module();
            else
                e.Skip();
        });

        populate_filters();
        refresh_results();
    }

private:
    wxTextCtrl*   m_search;
    wxChoice*     m_mfr;
    wxChoice*     m_tag;
    wxListBox*    m_results;
    wxButton*     m_btn_add;
    wxStaticText* m_status;
    std::vector<BrowserEntry> m_all;
    std::vector<int>          m_filtered_idx;

    // Read modules_db.json using raw byte stream to preserve UTF-8 correctly
    void load_db()
    {
        wxString p1 = g_exedir + wxFILE_SEP_PATH + "modules_db.json";
        wxString p2;
        const char* up = std::getenv("USERPROFILE");
        if (up)
            p2 = wxString::FromUTF8(std::string(up) +
                 "\\Documents\\cardinal-accessible-wx\\modules_db.json");

        std::string path;
        if      (wxFileExists(p1)) path = p1.ToStdString();
        else if (wxFileExists(p2)) path = p2.ToStdString();
        else return;

        // std::ifstream reads raw bytes; nlohmann/json handles UTF-8 directly
        std::ifstream ifs(path);
        if (!ifs.is_open()) return;
        try {
            auto j = json::parse(ifs);
            for (auto& m : j) {
                BrowserEntry e;
                e.plugin       = m.value("plugin", "");
                e.model        = m.value("model", "");
                e.name         = m.value("name", e.model);
                e.manufacturer = m.value("manufacturer", "");
                if (m.contains("tags") && m["tags"].is_array())
                    for (auto& t : m["tags"])
                        e.tags.push_back(t.get<std::string>());
                m_all.push_back(std::move(e));
            }
        } catch (...) {}
    }

    void populate_filters()
    {
        std::set<std::string> mfrs, tags;
        for (auto& e : m_all) {
            if (!e.manufacturer.empty()) mfrs.insert(e.manufacturer);
            for (auto& t : e.tags) tags.insert(t);
        }
        m_mfr->Append(tr("All"));
        for (auto& s : mfrs) m_mfr->Append(wxString::FromUTF8(s));
        m_mfr->SetSelection(0);

        m_tag->Append(tr("All"));
        for (auto& s : tags) m_tag->Append(wxString::FromUTF8(s));
        m_tag->SetSelection(0);
    }

    void refresh_results()
    {
        wxString search = m_search->GetValue().Lower();
        wxString mfr    = m_mfr->GetStringSelection();
        wxString tag    = m_tag->GetStringSelection();
        wxString all_lbl = tr("All");

        m_filtered_idx.clear();
        m_results->Clear();

        if (m_all.empty()) {
            m_status->SetLabel(tr("modules_db.json not found. "
                "Copy it to C:\\Program Files\\Cardinal\\."));
            return;
        }

        for (int i = 0; i < (int)m_all.size(); ++i) {
            const auto& e = m_all[i];

            if (mfr != all_lbl && wxString::FromUTF8(e.manufacturer) != mfr)
                continue;

            if (tag != all_lbl) {
                bool has = false;
                for (auto& t : e.tags) if (wxString::FromUTF8(t) == tag) { has = true; break; }
                if (!has) continue;
            }

            if (!search.IsEmpty()) {
                // Build haystack from std::string to avoid encoding issues
                std::string hay = e.name + " " + e.model + " " +
                                  e.plugin + " " + e.manufacturer;
                for (auto& t : e.tags) hay += " " + t;
                if (!wxString::FromUTF8(hay).Lower().Contains(search)) continue;
            }

            m_filtered_idx.push_back(i);

            // Build label entirely from UTF-8 std::string, then convert once
            std::string lbl = e.name + " - " + e.manufacturer;
            if (!e.tags.empty()) {
                lbl += " (";
                for (int k = 0; k < (int)e.tags.size() && k < 3; ++k) {
                    if (k > 0) lbl += ", ";
                    lbl += e.tags[k];
                }
                lbl += ")";
            }
            m_results->Append(wxString::FromUTF8(lbl));
        }

        if (!m_filtered_idx.empty()) m_results->SetSelection(0);
        m_status->SetLabel(wxString::Format(tr("%d modules found."),
                           (int)m_filtered_idx.size()));
    }

    void set_status(const wxString& s)
    {
        if (auto* f = wxDynamicCast(wxGetTopLevelParent(this), wxFrame))
            f->SetStatusText(s);
    }

    void add_module()
    {
        int sel = m_results->GetSelection();
        if (sel < 0 || sel >= (int)m_filtered_idx.size()) {
            set_status(tr("No module selected."));
            return;
        }
        const auto& e = m_all[m_filtered_idx[sel]];
        json body = {{"pluginSlug", e.plugin}, {"moduleSlug", e.model}};

        auto res = http().Post("/api/modules", body.dump(), "application/json");
        if (res && res->status == 200) {
            announce(tr("Added: ") + wxString::FromUTF8(e.name));
            if (onModuleAdded) onModuleAdded();
        } else {
            std::string err = "Module add error.";
            if (!res) {
                err += " (no response)";
            } else {
                err += " (HTTP " + std::to_string(res->status) + ")";
                try {
                    auto j = json::parse(res->body);
                    if (j.contains("error"))
                        err += ": " + j["error"].get<std::string>();
                } catch (...) {}
            }
            announce(wxString::FromUTF8(err));
        }
    }
};

// Forward declarations (defined after AudioPanel, used inside it)
static bool start_engine();
static void stop_engine();

// ── AudioPanel (tab 4) ────────────────────────────────────────────────────────

class AudioPanel : public wxPanel
{
    static constexpr int SR_COUNT = 6;
    static constexpr int SR_VALUES[SR_COUNT] = {44100, 48000, 88200, 96000, 176400, 192000};
    static constexpr int BS_COUNT = 7;
    static constexpr int BS_VALUES[BS_COUNT] = {64, 128, 256, 512, 1024, 2048, 4096};

public:
    AudioPanel(wxWindow* parent) : wxPanel(parent), m_restarting(false)
    {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(this, wxID_ANY, tr("Audio settings:")),
                   0, wxALL, 6);

        auto* grid = new wxFlexGridSizer(4, 2, 6, 10);

        grid->Add(new wxStaticText(this, wxID_ANY, tr("Driver:")), 0, wxALIGN_CENTER_VERTICAL);
        m_driver = new wxChoice(this, wxID_ANY);
        m_driver->Append("wasapi");
        m_driver->Append("asio");
        m_driver->SetSelection(0);
        grid->Add(m_driver);

        grid->Add(new wxStaticText(this, wxID_ANY, tr("Device:")), 0, wxALIGN_CENTER_VERTICAL);
        m_device = new wxChoice(this, wxID_ANY);
        grid->Add(m_device);

        grid->Add(new wxStaticText(this, wxID_ANY, tr("Sample rate:")), 0, wxALIGN_CENTER_VERTICAL);
        m_sr = new wxChoice(this, wxID_ANY);
        for (int i = 0; i < SR_COUNT; ++i)
            m_sr->Append(wxString::Format("%d Hz", SR_VALUES[i]));
        m_sr->SetSelection(1);
        grid->Add(m_sr);

        grid->Add(new wxStaticText(this, wxID_ANY, tr("Buffer size:")), 0, wxALIGN_CENTER_VERTICAL);
        m_bs = new wxChoice(this, wxID_ANY);
        for (int i = 0; i < BS_COUNT; ++i)
            m_bs->Append(wxString::Format("%d frames", BS_VALUES[i]));
        m_bs->SetSelection(3);
        grid->Add(m_bs);

        sizer->Add(grid, 0, wxALL, 8);

        m_status = new wxStaticText(this, wxID_ANY, "");
        sizer->Add(m_status, 0, wxALL, 6);

        auto* btn_row = new wxBoxSizer(wxHORIZONTAL);
        auto* btn_refresh  = new wxButton(this, wxID_ANY, tr("Read current configuration"));
        auto* btn_save     = new wxButton(this, wxID_ANY, tr("Save settings"));
        m_btn_restart      = new wxButton(this, wxID_ANY, tr("Restart DSP"));
        btn_row->Add(btn_refresh,    0, wxRIGHT, 6);
        btn_row->Add(btn_save,       0, wxRIGHT, 6);
        btn_row->Add(m_btn_restart,  0);
        sizer->Add(btn_row, 0, wxALL, 6);

        SetSizer(sizer);

        btn_refresh   ->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { load_config(); });
        btn_save      ->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save_config(); });
        m_btn_restart ->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { do_restart(); });
        m_driver      ->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { load_devices(); });
        m_reconnect_timer.Bind(wxEVT_TIMER, &AudioPanel::on_restart_timer, this);

        load_config();
    }

private:
    wxChoice*     m_driver;
    wxChoice*     m_device;
    wxChoice*     m_sr;
    wxChoice*     m_bs;
    wxStaticText* m_status;
    wxButton*     m_btn_restart;
    wxTimer       m_reconnect_timer;
    bool          m_restarting;
    std::vector<std::string> m_device_names;

    void load_config()
    {
        auto cfg = http_get_json("/api/audio/config");
        if (!cfg) { m_status->SetLabel(tr("Cardinal not reachable.")); return; }

        m_driver->SetSelection(cfg->value("driver", "wasapi") == "asio" ? 1 : 0);

        int sr = cfg->value("sampleRate", 48000);
        for (int i = 0; i < SR_COUNT; ++i)
            if (SR_VALUES[i] == sr) { m_sr->SetSelection(i); break; }

        int bs = cfg->value("bufferSize", 512);
        for (int i = 0; i < BS_COUNT; ++i)
            if (BS_VALUES[i] == bs) { m_bs->SetSelection(i); break; }

        load_devices(cfg->value("device", ""));
        m_status->SetLabel(tr("Configuration loaded."));
    }

    void load_devices(const std::string& selected = "")
    {
        m_device->Clear();
        m_device_names.clear();
        auto devs = http_get_json("/api/audio/devices");
        if (!devs) return;
        std::string drv = m_driver->GetSelection() == 1 ? "ASIO" : "WASAPI";
        for (auto& d : *devs) {
            if (d.value("driver", "") != drv) continue;
            for (auto& dev : d["devices"]) {
                std::string name = dev.value("name", "");
                m_device_names.push_back(name);
                m_device->Append(wxString::FromUTF8(name));
            }
        }
        for (int i = 0; i < (int)m_device_names.size(); ++i)
            if (m_device_names[i] == selected) { m_device->SetSelection(i); return; }
        if (m_device->GetCount() > 0) m_device->SetSelection(0);
    }

    void save_config()
    {
        int dev_sel = m_device->GetSelection();
        if (dev_sel < 0 || dev_sel >= (int)m_device_names.size()) {
            m_status->SetLabel(tr("No device selected."));
            return;
        }
        json body = {
            {"driver",     m_driver->GetSelection() == 1 ? "asio" : "wasapi"},
            {"device",     m_device_names[dev_sel]},
            {"sampleRate", SR_VALUES[m_sr->GetSelection()]},
            {"bufferSize", BS_VALUES[m_bs->GetSelection()]}
        };
        if (http_post_json("/api/audio/config", body))
            m_status->SetLabel(tr("Saved."));
        else
            m_status->SetLabel(tr("Save error."));
    }

    void do_restart()
    {
        // Save current settings first, then restart the engine
        save_config();
        m_status->SetLabel(tr("Restarting DSP..."));
        announce(tr("Restarting DSP..."));
        m_btn_restart->Enable(false);
        stop_engine();
        m_restarting = false;
        m_reconnect_timer.StartOnce(800);
    }

    void on_restart_timer(wxTimerEvent&)
    {
        if (!m_restarting) {
            // Phase 1: engine is stopped, now start it
            if (!start_engine()) {
                m_status->SetLabel(tr("DSP restart failed."));
                announce(tr("DSP restart failed."));
                m_btn_restart->Enable(true);
                return;
            }
            m_restarting = true;
            m_reconnect_timer.StartOnce(1000);
        } else {
            // Phase 2: poll until HTTP responds
            auto res = http().Get("/api/modules");
            if (!res || res->status != 200) {
                m_reconnect_timer.StartOnce(1000);
                return;
            }
            m_restarting = false;
            load_config();
            m_btn_restart->Enable(true);
            announce(tr("DSP restarted."));
        }
    }
};

// ── Engine process management ─────────────────────────────────────────────────

static wxProcess* gEngineProcess = nullptr;
static long       gEnginePid     = 0;

static bool start_engine()
{
    wxString candidate = g_exedir + wxFILE_SEP_PATH + "CardinalNative.exe";
    if (!wxFileExists(candidate))
        candidate = "C:\\Program Files\\Cardinal\\CardinalNative.exe";

    if (!wxFileExists(candidate)) {
        wxMessageBox(
            tr("CardinalNative.exe not found.\n\n"
               "Install Cardinal in C:\\Program Files\\Cardinal\\\n"
               "or place CardinalNative.exe in the same folder as this executable."),
            tr("Cardinal Accessible - Startup Error"),
            wxOK | wxICON_ERROR);
        return false;
    }
    gEngineProcess = new wxProcess();
    gEnginePid = wxExecute(candidate + " --hidden",
                           wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE,
                           gEngineProcess);
    return gEnginePid > 0;
}

static void stop_engine()
{
    if (gEnginePid > 0) {
        wxKillError err;
        wxKill(gEnginePid, wxSIGTERM, &err);
        gEnginePid = 0;
    }
}

// ── MainFrame ─────────────────────────────────────────────────────────────────

class MainFrame : public wxFrame
{
public:
    MainFrame() : wxFrame(nullptr, wxID_ANY, "Cardinal Accessible",
                          wxDefaultPosition, wxSize(900, 680))
    {
        // ── Menu bar ──────────────────────────────────────────────────────────
        auto* mbar      = new wxMenuBar();
        auto* file_menu = new wxMenu();
        file_menu->Append(wxID_NEW,  tr("New patch\tCtrl+N"));
        file_menu->Append(wxID_OPEN, tr("Open patch from file...\tCtrl+O"));
        file_menu->Append(wxID_SAVE, tr("Save patch to file...\tCtrl+S"));
        file_menu->AppendSeparator();
        file_menu->Append(wxID_EXIT, tr("Quit"));
        mbar->Append(file_menu, tr("File"));

        // Language menu — built-in + external JSON files found in lang/
        auto* lang_menu = new wxMenu();
        for (int i = 0; i < (int)g_available_langs.size(); ++i) {
            auto* item = lang_menu->AppendRadioItem(ID_LANG_BASE + i,
                                                    g_available_langs[i].name);
            if (g_available_langs[i].code == g_lang_code)
                item->Check(true);
        }
        mbar->Append(lang_menu, tr("Language"));

        auto* opts_menu = new wxMenu();
        opts_menu->AppendCheckItem(ID_SILENT_REFRESH, tr("Silent refresh"));
        mbar->Append(opts_menu, tr("Options"));
        Bind(wxEVT_MENU, [](wxCommandEvent& e) {
            g_silent_refresh = e.IsChecked();
        }, ID_SILENT_REFRESH);

        SetMenuBar(mbar);

        // ── Notebook ──────────────────────────────────────────────────────────
        auto* nb = new wxNotebook(this, wxID_ANY);
        m_params  = new ParamsPanel(nb);
        m_cables  = new CablesPanel(nb);
        m_browser = new BrowserPanel(nb);
        m_audio   = new AudioPanel(nb);
        nb->AddPage(m_params,  tr("Parameters (Ctrl+1)"));
        nb->AddPage(m_cables,  tr("Cables (Ctrl+2)"));
        nb->AddPage(m_browser, tr("Browser (Ctrl+3)"));
        nb->AddPage(m_audio,   tr("Audio (Ctrl+4)"));

        // After adding a module from Browser, defer the refresh one event-loop cycle
        // so NVDA has time to read "Added: X" before "N modules loaded." overwrites it.
        m_browser->onModuleAdded = [this]() {
            CallAfter([this]() { refresh_all(); });
        };

        // Frame layout: notebook fills the window; g_status_label sits below it.
        // wxStaticText triggers EVENT_OBJECT_NAMECHANGE on SetLabel(), which NVDA
        // picks up. announce() also fires EVENT_SYSTEM_ALERT for an immediate read.
        auto* frame_sz = new wxBoxSizer(wxVERTICAL);
        frame_sz->Add(nb, 1, wxEXPAND);
        g_status_label = new wxStaticText(this, wxID_ANY, "");
        frame_sz->Add(g_status_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
        SetSizer(frame_sz);

        CreateStatusBar(2);
        { int w[] = {-1, 80}; GetStatusBar()->SetStatusWidths(2, w); }
        announce(tr("Connecting to Cardinal..."));

        // ── Tab accelerators Ctrl+1..4 ────────────────────────────────────────
        wxAcceleratorEntry acc[4];
        acc[0].Set(wxACCEL_CTRL, '1', 10001);
        acc[1].Set(wxACCEL_CTRL, '2', 10002);
        acc[2].Set(wxACCEL_CTRL, '3', 10003);
        acc[3].Set(wxACCEL_CTRL, '4', 10004);
        SetAcceleratorTable(wxAcceleratorTable(4, acc));
        for (int i = 0; i < 4; ++i)
            Bind(wxEVT_MENU, [nb, i](wxCommandEvent&) { nb->SetSelection(i); }, 10001 + i);

        // ── File menu bindings ────────────────────────────────────────────────
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { new_patch(); },  wxID_NEW);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { open_patch(); }, wxID_OPEN);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { save_patch(); }, wxID_SAVE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); },      wxID_EXIT);

        // ── Language menu bindings ────────────────────────────────────────────
        for (int i = 0; i < (int)g_available_langs.size(); ++i) {
            Bind(wxEVT_MENU, [i](wxCommandEvent&) {
                on_language_change(i);
            }, ID_LANG_BASE + i);
        }

        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::on_close, this);
        Show();

        m_connect_timer.Bind(wxEVT_TIMER, &MainFrame::on_connect_timer, this);
        m_connect_timer.StartOnce(500);
    }

private:
    ParamsPanel*  m_params;
    CablesPanel*  m_cables;
    BrowserPanel* m_browser;
    AudioPanel*   m_audio;
    wxTimer       m_connect_timer;
    WsClient      m_ws;
    std::atomic<bool> m_ws_active{false};

    static void on_language_change(int idx)
    {
        if (idx < 0 || idx >= (int)g_available_langs.size()) return;
        const std::string& code = g_available_langs[idx].code;
        if (code == g_lang_code) return;

        wxConfig::Get()->Write("language", wxString::FromUTF8(code));
        wxConfig::Get()->Flush();

        // Bilingual message: we don't know which direction the user is switching
        wxMessageBox(
            "Language will be applied on next startup.\n\n"
            "La lingua verra' applicata al prossimo avvio.",
            "Restart / Riavvio",
            wxOK | wxICON_INFORMATION);
    }

    void new_patch()
    {
        json body = {{"version","2.5.2"},
                     {"modules", json::array()},
                     {"cables",  json::array()}};
        if (http_post_json("/api/patch/load", body)) {
            announce(tr("New patch created."));
            refresh_all();
        }
    }

    void open_patch()
    {
        wxFileDialog dlg(this, tr("Open Cardinal patch"), "", "",
                         tr("Cardinal patches (*.vcv)|*.vcv|All files (*.*)|*.*"),
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_CANCEL) return;
        json body = {{"path", dlg.GetPath().utf8_string()}};
        if (http_post_json("/api/patch/load", body)) {
            announce(tr("Patch loaded: ") + dlg.GetPath());
            refresh_all();
        } else {
            announce(tr("Patch load error."));
        }
    }

    void save_patch()
    {
        auto result = http_get_json("/api/patch");
        if (!result) { announce(tr("Cannot retrieve patch from Cardinal.")); return; }

        wxFileDialog dlg(this, tr("Save Cardinal patch"), "", "patch.vcv",
                         tr("Cardinal patches (*.vcv)|*.vcv"),
                         wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() == wxID_CANCEL) return;

        wxFile file(dlg.GetPath(), wxFile::write);
        if (!file.IsOpened()) { announce(tr("Cannot open file for writing.")); return; }
        std::string s = result->dump(2);
        file.Write(s.data(), s.size());
        announce(tr("Patch saved: ") + dlg.GetPath());
    }

    void refresh_all()
    {
        m_params->refresh();
        m_cables->refresh();
    }

    void on_connect_timer(wxTimerEvent&)
    {
        auto res = http().Get("/api/modules");
        if (!res || res->status != 200) {
            announce(tr("Cardinal not responding - retrying..."));
            m_connect_timer.StartOnce(1500);
            return;
        }
        announce(tr("Connected to Cardinal."));
        refresh_all();
        start_ws();
    }

    void start_ws()
    {
        m_ws_active = false;
        m_ws.stop();
        m_ws_active = true;
        m_ws.start(
            [this](const std::string& msg) {
                if (!m_ws_active) return;
                wxTheApp->CallAfter([this, msg]() {
                    if (m_ws_active) on_ws_message(msg);
                });
            },
            [this](bool ok) {
                wxTheApp->CallAfter([this, ok]() {
                    if (m_ws_active)
                        SetStatusText(ok ? wxString::FromUTF8("● Live")
                                        : wxString::FromUTF8("○ WS"), 1);
                });
            }
        );
    }

    void on_ws_message(const std::string& msg)
    {
        try {
            auto arr = json::parse(msg);
            if (!arr.is_array()) return;
            for (auto& item : arr) {
                int64_t mid = item.value("m", int64_t(0));
                int     pid = item.value("p", 0);
                double  val = item["v"].is_null() ? 0.0 : item["v"].get<double>();
                m_params->update_param(mid, pid, val);
            }
        } catch (...) {}
    }

    void on_close(wxCloseEvent& event)
    {
        m_ws_active = false;
        m_ws.stop();
        stop_engine();
        event.Skip();
    }
};

// ── App ───────────────────────────────────────────────────────────────────────

class CardinalAccessibleApp : public wxApp
{
public:
    bool OnInit() override
    {
        SetVendorName("Cardinal");
        SetAppName("CardinalAccessibleUI");

        g_exedir = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
        load_catalog();

        // Load saved language, default English
        wxString saved;
        std::string lang_code = "en";
        if (wxConfig::Get()->Read("language", &saved))
            lang_code = saved.ToStdString();

        scan_languages();
        apply_language(lang_code);

        if (!start_engine()) return false;
        new MainFrame();
        // init_nvda_client() after MainFrame so nvdaHelperRemote.dll is injected
        init_nvda_client();
        return true;
    }

    int OnExit() override
    {
        stop_engine();
        return 0;
    }
};

wxIMPLEMENT_APP(CardinalAccessibleApp);
