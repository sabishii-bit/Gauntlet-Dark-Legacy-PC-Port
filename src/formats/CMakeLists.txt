# Readers for the console-specific asset files. The game never links this; the unpack
# tool converts those files into standard formats that the engine loads instead.
add_library(formats STATIC
    ModelArchive.h
    ModelArchive.cpp
    GcTexture.h
    GcTexture.cpp
    FontFile.h
    FontFile.cpp
    TextRom.h
    TextRom.cpp
    JsonWriter.h
    JsonWriter.cpp
    AudioRom.h
    AudioRom.cpp
    SoundBank.h
    SoundBank.cpp
    WavWriter.h
    WavWriter.cpp
    GeometryStream.h
    GeometryStream.cpp
    AnimationTree.h
    AnimationTree.cpp
    ObjWriter.h
    ObjWriter.cpp
    PlayerDataWad.h
    PlayerDataWad.cpp
    WorldFile.h
    WorldFile.cpp)

add_library(gdl::formats ALIAS formats)

target_link_libraries(formats PUBLIC gdl::engine)
gdl_apply_compiler_options(formats)
set_target_properties(formats PROPERTIES FOLDER "formats")
