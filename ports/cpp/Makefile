# Full-research C++ backup. Prefer: powershell -File build.ps1
# Minimal fallback (zlib only) is not full parity — use build.ps1.

.PHONY: all clean

all:
	powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1

clean:
	rm -f bav-cpp bav-cpp.exe libzstd.dll
