#define MyAppName "Postmodular"
#define MyAppVersion "26.02"
#define MyAppPublisher "Luca Casarotti"
#define MyAppURL "https://github.com/lcasarotti/Postmodular"
#define DistDir "..\..\dist"

[Setup]
ArchitecturesInstallIn64BitMode=x64compatible
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={commonpf64}\Postmodular
DisableDirPage=yes
DisableWelcomePage=no
LicenseFile=..\..\LICENSE
OutputBaseFilename=Postmodular-{#MyAppVersion}-installer
OutputDir=.
UsePreviousAppDir=no
Compression=lzma2/ultra64
SolidCompression=yes
SetupIconFile=..\..\utils\distrho.ico
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={commonpf64}\Postmodular\PostmodularAccessibleUI.exe
ShowLanguageDialog=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl";
Name: "it"; MessagesFile: "compiler:Languages\Italian.isl";

[CustomMessages]
; --- Installation types ---
en.TypeFull=Full installation (VST3 + CLAP + Standalone + UI)
it.TypeFull=Installazione completa (VST3 + CLAP + Standalone + UI)
en.TypeVst=VST3 plugin + UI only
it.TypeVst=Solo plugin VST3 + UI
en.TypeStandalone=Standalone + UI only
it.TypeStandalone=Solo Standalone + UI
en.TypeCustom=Custom
it.TypeCustom=Personalizzata

; --- Component descriptions ---
en.CompUI=Postmodular Accessible UI
it.CompUI=Interfaccia accessibile Postmodular
en.CompVST3=VST3 plugin
it.CompVST3=Plugin VST3
en.CompCLAP=CLAP plugins (FX + Synth)
it.CompCLAP=Plugin CLAP (FX + Synth)
en.CompNative=Standalone engine
it.CompNative=Motore standalone

; --- Post-install run checkbox ---
en.RunUI=Launch Postmodular Accessible UI
it.RunUI=Avvia Postmodular Accessible UI

; --- Desktop shortcut ---
en.DesktopIcon=Create desktop shortcut for Postmodular Standalone
it.DesktopIcon=Crea icona sul desktop per Postmodular Standalone

; --- Start Menu shortcut comment ---
en.ShortcutComment=Postmodular — accessible interface for modular synthesizer
it.ShortcutComment=Postmodular — interfaccia accessibile per sintetizzatore modulare

[Types]
Name: "full";       Description: "{cm:TypeFull}";
Name: "vst";        Description: "{cm:TypeVst}";
Name: "standalone"; Description: "{cm:TypeStandalone}";
Name: "custom";     Description: "{cm:TypeCustom}"; Flags: iscustom;

[Components]
; UI is fixed — installed with every configuration
Name: ui;     Description: "{cm:CompUI}";    Types: full vst standalone custom; Flags: fixed;
Name: vst3;   Description: "{cm:CompVST3}";  Types: full vst custom;
Name: clap;   Description: "{cm:CompCLAP}";  Types: full custom;
Name: native; Description: "{cm:CompNative}"; Types: full standalone custom;

[Files]
; --- UI (always) ---
Source: "{#DistDir}\gui\PostmodularAccessibleUI.exe"; DestDir: "{commonpf64}\Postmodular"; Components: ui; Flags: ignoreversion;
Source: "{#DistDir}\data\catalog.json";               DestDir: "{commonpf64}\Postmodular"; Components: ui; Flags: ignoreversion;
Source: "{#DistDir}\data\modules_db.json";            DestDir: "{commonpf64}\Postmodular"; Components: ui; Flags: ignoreversion;

; --- Standalone engine + resources ---
Source: "{#DistDir}\native\PostmodularNative.exe";    DestDir: "{commonpf64}\Postmodular"; Components: native; Flags: ignoreversion;
Source: "{#DistDir}\resources\*"; DestDir: "{commonpf64}\Postmodular\resources"; Components: native; \
    Flags: recursesubdirs createallsubdirs ignoreversion;

; --- VST3 DLL + risorse nel bundle ---
Source: "{#DistDir}\vst3\Postmodular.vst3"; \
    DestDir: "{commoncf64}\VST3\Postmodular.vst3\Contents\x86_64-win"; Components: vst3; Flags: ignoreversion;
Source: "{#DistDir}\resources\*"; \
    DestDir: "{commoncf64}\VST3\Postmodular.vst3\Contents\Resources"; Components: vst3; \
    Flags: recursesubdirs createallsubdirs ignoreversion;

; --- CLAP DLLs ---
Source: "{#DistDir}\clap\PostmodularFX.clap";    DestDir: "{commoncf64}\CLAP"; Components: clap; Flags: ignoreversion;
Source: "{#DistDir}\clap\PostmodularSynth.clap"; DestDir: "{commoncf64}\CLAP"; Components: clap; Flags: ignoreversion;

; --- CLAP: PluginManifests nel percorso di fallback ---
Source: "{#DistDir}\resources\PluginManifests\*"; \
    DestDir: "{commoncf64}\Postmodular\PluginManifests"; Components: clap; \
    Flags: recursesubdirs createallsubdirs ignoreversion;

[Tasks]
Name: desktopicon; Description: "{cm:DesktopIcon}"; Components: native;

[Icons]
Name: "{commonprograms}\{#MyAppName}"; \
    Filename: "{commonpf64}\Postmodular\PostmodularAccessibleUI.exe"; \
    IconFilename: "{commonpf64}\Postmodular\PostmodularAccessibleUI.exe"; \
    WorkingDir: "{commonpf64}\Postmodular"; \
    Comment: "{cm:ShortcutComment}";
Name: "{commondesktop}\{#MyAppName}"; \
    Filename: "{commonpf64}\Postmodular\PostmodularAccessibleUI.exe"; \
    IconFilename: "{commonpf64}\Postmodular\PostmodularAccessibleUI.exe"; \
    WorkingDir: "{commonpf64}\Postmodular"; \
    Comment: "{cm:ShortcutComment}"; \
    Tasks: desktopicon; Components: native;

[Run]
Filename: "{commonpf64}\Postmodular\PostmodularAccessibleUI.exe"; \
    Description: "{cm:RunUI}"; \
    Flags: nowait postinstall skipifsilent; \
    Components: ui;
