"""Package the standalone SDL launcher for Finder without duplicating game data."""
from pathlib import Path
import plistlib
import shutil
import subprocess


def stage_launcher(package):
    package = Path(package).resolve()
    contents = package / "FM3 Launcher.app" / "Contents"
    executable = contents / "MacOS" / "fm3-launcher"
    executable.parent.mkdir(parents=True, exist_ok=True)
    (contents / "Resources").mkdir(exist_ok=True)
    shutil.copy2(package / "fm3-launcher", executable)
    executable.chmod(0o755)
    with (contents / "Info.plist").open("wb") as output:
        plistlib.dump({"CFBundleExecutable": "fm3-launcher",
                       "CFBundleIdentifier": "org.hl360.forza3.launcher",
                       "CFBundleName": "FM3 Launcher",
                       "CFBundleDisplayName": "FM3 Launcher",
                       "CFBundlePackageType": "APPL",
                       "CFBundleVersion": "1",
                       "CFBundleShortVersionString": "0.1",
                       "LSMinimumSystemVersion": "14.0",
                       "NSHighResolutionCapable": True}, output)
    subprocess.run(["codesign", "--force", "--sign", "-", str(contents.parent)], check=True)
    return contents.parent
