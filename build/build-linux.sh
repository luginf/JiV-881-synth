ROOT=$(cd "$(dirname "$0")/.."; pwd)

"$ROOT/build/bin/JUCE/Projucer" --resave "$ROOT/VirtualJiV.jucer"

cd "$ROOT/Builds/LinuxMakefile"
make CONFIG=Release

# Strip the shipped binaries (Alan's request, 2026-09-08 - the unstripped Standalone alone was
# ~104MB in a Debug build; Release doesn't carry -g but still has a full symbol table). Done here
# rather than only as a manual dev habit so both local releases and CI get it automatically.
strip build/JiV-881 build/JiV-881.lv2/JiV-881.so build/JiV-881.vst3/Contents/x86_64-linux/JiV-881.so

# jiv881.a (JUCE_TARGET_SHARED_CODE) is a static archive of every object file, purely an
# intermediate link input for the targets above - already linked into all of them, never needed
# again afterwards. Alan noticed it ballooning the build/ folder (222MB) alongside the unstripped
# Standalone; removing it here keeps that down without touching `make clean`/incremental rebuilds
# (make just relinks it from the still-cached .o files next time it's needed).
rm -f build/JiV-881.a
