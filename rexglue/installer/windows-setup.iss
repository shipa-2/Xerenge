; The Windows installer as one .exe: the installer, its Qt libraries and the
; payload are unpacked to a temporary folder, the installer is run from there,
; and the folder is removed when it closes. Nothing is installed by this
; wrapper itself - the Qt installer does that, as it does from the AppImage.
;
;   iscc /DPackage=<package dir> /DVersion=<x.y.z> /O<out dir> windows-setup.iss

#ifndef Package
  #error Package (the folder with xerenge-installer.exe and payload\) is required
#endif
#ifndef Version
  #define Version "0.0.0"
#endif

[Setup]
AppName=Burnout Revenge (Xerenge)
AppVersion={#Version}
AppPublisher=Xerenge
CreateAppDir=no
Uninstallable=no
PrivilegesRequired=lowest
DisableWelcomePage=yes
DisableReadyPage=yes
DisableFinishedPage=yes
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2/max
SolidCompression=yes
OutputBaseFilename=Burnout_Revenge_Installer-win-amd64
WizardStyle=modern

[Files]
Source: "{#Package}\*"; DestDir: "{tmp}\xerenge"; Flags: recursesubdirs createallsubdirs ignoreversion

[Run]
Filename: "{tmp}\xerenge\xerenge-installer.exe"; WorkingDir: "{tmp}\xerenge"; Flags: waituntilterminated
