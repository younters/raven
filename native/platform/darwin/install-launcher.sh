#!/bin/sh
# Installs Raven.app, a launcher that opens Windows programs from Finder (double-click, Open With, or dropping one on its icon), and
# makes it the default application for .exe files. Each program runs in its own prefix, named after the program, under
# ~/.local/share/raven, with its output in ~/Library/Logs/Raven. Opening Raven.app itself asks for a program.
#
#   native/platform/darwin/install-launcher.sh [build-directory] [--no-default]
set -eu

SOURCE=$(cd "$(dirname "$0")/../../.." && pwd)
BUILD=$SOURCE/build-macos
DEFAULT=1
for argument in "$@"; do
	case $argument in
	--no-default) DEFAULT= ;;
	*) BUILD=$(cd "$argument" && pwd) ;;
	esac
done
RAVEN=$BUILD/native/raven
APP=$HOME/Applications/Raven.app
IDENTIFIER=org.raven.launcher

[ -x "$RAVEN" ] || { echo "install-launcher.sh: $RAVEN is not built" >&2; exit 1; }
mkdir -p "$HOME/Applications"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# An AppleScript applet receives Finder's open-document events, which a plain executable in a bundle would not.
cat > "$work/launcher.applescript" <<EOF
on launch_program(program)
	set programPath to POSIX path of program
	set command to "RAVEN_BINARY=" & quoted form of "$RAVEN" & " " & quoted form of "$SOURCE/native/platform/darwin/raven-apps" & " open " & quoted form of programPath
	do shell script command
end launch_program

on open programs
	repeat with program in programs
		launch_program(program)
	end repeat
end open

on run
	set program to choose file with prompt "Choose a Windows program to run with Raven:" of type {"exe", "com.microsoft.windows-executable"}
	launch_program(program)
end run
EOF

rm -rf "$APP"
osacompile -o "$APP" "$work/launcher.applescript"
plist=$APP/Contents/Info.plist
plutil -replace CFBundleIdentifier -string "$IDENTIFIER" "$plist"
plutil -replace CFBundleName -string Raven "$plist"
plutil -replace CFBundleDocumentTypes -json '[{"CFBundleTypeName":"Windows program","CFBundleTypeRole":"Viewer","LSHandlerRank":"Owner","LSItemContentTypes":["com.microsoft.windows-executable"],"CFBundleTypeExtensions":["exe"]}]' "$plist"
codesign --force --sign - "$APP" >/dev/null 2>&1
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP"

if [ -n "$DEFAULT" ]; then
	cat > "$work/default.swift" <<EOF
import AppKit
import UniformTypeIdentifiers
let done = DispatchSemaphore(value: 0)
var failure: Error?
NSWorkspace.shared.setDefaultApplication(at: URL(fileURLWithPath: "$APP"), toOpen: UTType("com.microsoft.windows-executable")!) { error in
	failure = error
	done.signal()
}
done.wait()
if let failure { print("setDefaultApplication: \(failure)"); exit(1) }
EOF
	swift "$work/default.swift"
fi

echo "Installed $APP for $RAVEN"
[ -z "$DEFAULT" ] || echo ".exe files now open with Raven; their prefixes are in ~/.local/share/raven, their logs in ~/Library/Logs/Raven"
