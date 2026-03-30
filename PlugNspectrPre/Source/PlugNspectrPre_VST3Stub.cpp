/*
  ==============================================================================
    PlugNspectrPre_VST3Stub.cpp

    This file has no functional code.  Its sole purpose is to give the
    PlugNspectrPre_VST3 DLL project at least one compiled C++ translation
    unit.  When MSVC compiles any .cpp with /MD or /MDd it embeds a
    /DEFAULTLIB linker directive in the resulting .obj.  That directive
    causes the linker to pull in msvcrt(d).lib, which in turn provides the
    DLL entry-point symbol _DllMainCRTStartup — resolving LNK2001.

    Do NOT define DllMain here; JUCE's VST3 wrapper already defines it
    inside include_juce_audio_processors.cpp (SharedCode).
  ==============================================================================
*/

// Nothing to declare — presence of this TU is the entire fix.
