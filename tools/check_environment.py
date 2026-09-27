"""Check the selected Windows/Unix ESP-IDF environment before building TinyDesk Shell."""
import os
import pathlib
import re
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
idf_path = os.environ.get("IDF_PATH")
if not idf_path:
    sys.exit("Select ESP-IDF 5.3.1, then open ESP-IDF: Open ESP-IDF Terminal.")
idf = pathlib.Path(idf_path) / "tools" / "idf.py"
if not idf.is_file():
    sys.exit("IDF_PATH does not contain tools/idf.py: " + str(idf))
result = subprocess.run([sys.executable, str(idf), "--version"], text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
print(result.stdout.strip())
if result.returncode or not re.search(r"ESP-IDF v?5\.3\.1(?:\D|$)", result.stdout):
    sys.exit("This package requires ESP-IDF 5.3.1. Select that installation in VS Code.")
config = root / "sdkconfig"
if config.exists():
    target = re.search(r'^CONFIG_IDF_TARGET="([^"]+)"', config.read_text(), re.M)
    if target and target.group(1) != "esp32c6":
        sys.exit("Wrong target in sdkconfig; run idf.py set-target esp32c6.")
print("Environment OK: ESP-IDF 5.3.1 / ESP32-C6")
