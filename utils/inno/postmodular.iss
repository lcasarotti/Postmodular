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

[Types]
Name: "full";       Description: "Installazione completa (VST3 + CLAP + Standalone + UI)";
Name: "vst";        Description: "Solo plugin VST3 + UI";
Name: "standalone"; Description: "Solo Standalone + UI";
Name: "custom";     Description: "Personalizzata"; Flags: iscustom;

[Components]
; UI is fixed — installed with every configuration
Name: ui;     Description: "Postmodular Accessible UI";   Types: full vst standalone custom; Flags: fixed;
Name: vst3;   Description: "Plugin VST3";                 Types: full vst custom;
Name: clap;   Description: "Plugin CLAP (FX + Synth)";   Types: full custom;
Name: native; Description: "Motore standalone";           Types: full standalone custom;

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
Source: "{#DistDir}\vst3\Cardinal.vst3"; \
    DestDir: "{commoncf64}\VST3\Cardinal.vst3\Contents\x86_64-win"; Components: vst3; Flags: ignoreversion;
Source: "{#DistDir}\resources\*"; \
    DestDir: "{commoncf64}\VST3\Cardinal.vst3\Contents\Resources"; Components: vst3; \
    Flags: recursesubdirs createallsubdirs ignoreversion;

; --- CLAP DLLs ---
Source: "{#DistDir}\clap\CardinalFX.clap";    DestDir: "{commoncf64}\CLAP"; Components: clap; Flags: ignoreversion;
Source: "{#DistDir}\clap\CardinalSynth.clap"; DestDir: "{commoncf64}\CLAP"; Components: clap; Flags: ignoreversion;

; --- CLAP: PluginManifests nel percorso di fallback ---
Source: "{#DistDir}\resources\PluginManifests\*"; \
    DestDir: "{commoncf64}\Cardinal\PluginManifests"; Components: clap; \
    Flags: recursesubdirs createallsubdirs ignoreversion;

[Icons]
Name: "{commonprograms}\{#MyAppName}"; \
    Filename: "{commonpf64}\Postmodular\PostmodularAccessibleUI.exe"; \
    IconFilename: "{commonpf64}\Postmodular\PostmodularAccessibleUI.exe"; \
    WorkingDir: "{commonpf64}\Postmodular"; \
    Comment: "Postmodular — interfaccia accessibile per sintetizzatore modulare";

[Run]
; Offer to launch the UI immediately after install
Filename: "{commonpf64}\Postmodular\PostmodularAccessibleUI.exe"; \
    Description: "Avvia Postmodular Accessible UI"; \
    Flags: nowait postinstall skipifsilent; \
    Components: ui;
