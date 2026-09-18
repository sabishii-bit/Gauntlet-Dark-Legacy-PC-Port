#pragma once

#include <filesystem>
#include <string_view>

#include "engine/io/File.h"

#include "TestSupport.h"

namespace gdl::test {

/**
 * A level for the world scene and its animators: under one group at (10, 0, 0) stand a lit
 * wall (texture 0), a translucent window (texture 1), a glowing copy of the wall, a
 * lightmapped floor (texture 0 under lightmap 2), a torch flame on the external texture 3
 * (sorted, glowing, leaving depth unwritten), a particle marker wearing the wall's mesh,
 * two sorted panes near and far, a spinning group whose blade turns with it over four
 * frames, and a brazier marker where the level's torch flames burn; one object has no mesh
 * at all. Three triggers drive it: one at the torch that wants the first realm's crystals
 * and fades it, one at the spinning group that plays its turn and chains to a third at the
 * far pane.
 */
inline std::filesystem::path sampleLevel(std::string_view name) {
    const auto dir = scratchDirectory(name);
    std::filesystem::create_directories(dir / "models");
    std::filesystem::create_directories(dir / "textures");
    writeTextFile(dir / "models/000_WALL.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nvn 0 1 0\n"
                  "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\n");
    writeTextFile(dir / "models/001_WINDOW.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 -1 0\nusemtl tex1\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "models/002_FLOOR.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 0 1\nvt 0 0\nvt 1 0\nvt 0 1\n"
                  "vl 0.5 0.5\nvl 1 0.5\nvl 0.5 1\nvn 0 1 0\n"
                  "usemtl tex0_lm2\nf 1/1/1 2/2/1 3/3/1\n");
    writeTextFile(dir / "models/003_FLAME.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex3\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "models/004_BLADE.obj",
                  "v 1 0 0\nv 2 0 0\nv 1 1 0\nvn 0 1 0\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "models/005_PANE.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 1 0\nusemtl tex1\nf 1//1 2//1 3//1\n");
    // A prelit wall: its vertices carry their own dim colour.
    writeTextFile(dir / "models/006_LIT.obj",
                  "v 0 0 0 0.2 0.2 0.2\nv 1 0 0 0.2 0.2 0.2\nv 0 1 0 0.2 0.2 0.2\nvn 0 1 0\n"
                  "usemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({
  "source": "/sample/",
  "objects": [
    {"index": 0, "name": "WALL", "file": "models/000_WALL.obj", "meshTriangles": 1},
    {"index": 1, "name": "WINDOW", "file": "models/001_WINDOW.obj", "meshTriangles": 1},
    {"index": 2, "name": "FLOOR", "file": "models/002_FLOOR.obj", "meshTriangles": 1},
    {"index": 3, "name": "FLAME", "file": "models/003_FLAME.obj", "meshTriangles": 1},
    {"index": 4, "name": "BLADE", "file": "models/004_BLADE.obj", "meshTriangles": 1},
    {"index": 5, "name": "PANE", "file": "models/005_PANE.obj", "meshTriangles": 1},
    {"index": 6, "name": "LIT", "file": "models/006_LIT.obj", "meshTriangles": 1}
  ]
})");
    writeFile(dir / "textures/000_STONE.png", kTinyPng);
    writeFile(dir / "textures/001_GLASS.png", kTinyPng);
    writeFile(dir / "textures/002_LM.png", kTinyPng);
    // The torch's texture is external: flagged, named, but without an image of its own.
    writeTextFile(dir / "textures.json", R"({
  "source": "/sample/",
  "defs": [],
  "bitmaps": [
    {"index": 0, "name": "STONE", "file": "textures/000_STONE.png", "width": 2, "height": 2,
     "format": 50, "flags": 0, "halfResolution": false, "frames": 0},
    {"index": 1, "name": "GLASS", "file": "textures/001_GLASS.png", "width": 2, "height": 2,
     "format": 50, "flags": 128, "halfResolution": false, "frames": 0},
    {"index": 2, "name": "LM", "file": "textures/002_LM.png", "width": 2, "height": 2,
     "format": 146, "flags": 0, "halfResolution": false, "frames": 0},
    {"index": 3, "name": "TORCHB", "file": "textures/003_TORCHB.png", "width": 2, "height": 2,
     "format": 0, "flags": 160, "halfResolution": false, "frames": 0}
  ]
})");
    writeTextFile(dir / "world.json", R"({
  "objects": [
    {"name": "GROUP", "position": [10, 0, 0], "next": -1, "child": 1},
    {"name": "WINDOW", "position": [0, 5, 0], "next": 2, "child": -1},
    {"name": "WALL", "position": [1, 0, 0], "next": 3, "child": -1},
    {"name": "NOTHING", "position": [0, 0, 9], "next": 4, "child": -1},
    {"name": "WALL", "position": [2, 0, 0], "next": 5, "child": -1, "objectFlags": 8388608},
    {"name": "FLOOR", "position": [0, 0, 20], "next": 6, "child": -1},
    {"name": "FLAME", "position": [0, 0, 30], "next": 7, "child": -1, "objectFlags": 8390784},
    {"name": "WALL", "position": [0, 0, 40], "next": 8, "child": -1, "flags": 2048},
    {"name": "SPIN", "position": [0, 0, 50], "next": 9, "child": 10, "flags": 4096},
    {"name": "PANE", "position": [0, 0, 100], "next": 11, "child": -1, "objectFlags": 2048},
    {"name": "BLADE", "position": [0, 0, 0], "next": -1, "child": -1},
    {"name": "PANE", "position": [0, 0, 60], "next": 12, "child": -1, "objectFlags": 2048},
    {"name": "L1PSYSE_BRAZIER", "position": [0, 1, 70], "next": -1, "child": -1, "flags": 2048}
  ],
  "animations": [
    {"object": 8, "frames": 4, "state": 257, "start": 0,
     "track": {"flags": 2, "frames": [0, 3], "values": [0, 1]}}
  ],
  "particles": [
    {"id": "E", "preset": 5, "flags": 648, "flagMask": 648, "enables": 355297,
     "particleLife": [0.2, 0.22], "angle": 80, "texture": "P_TORCH",
     "direction": [0, 1, 0], "volume": [0.1, 0.3, 0.1], "rate": [25, 25, 25, 25],
     "gravity": -0.38, "speed": 4, "rgba": [0, 16777215, 16777215, 0],
     "width": [2.8, 2, 2, 0.1]}
  ],
  "itemInfos": [
    {"type": 5, "subtype": 24, "name": "BRIDGEPAD", "radius": 3, "height": 1}
  ],
  "itemInstances": [
    {"info": 0, "minPlayers": 1, "position": [10, 0, 30], "rotation": [0, 0, 0],
     "params": [6, 0, 114, 0, 4, 0, 1, 0, 0, 0, 0, 0]},
    {"info": 0, "minPlayers": 1, "position": [10, 0, 50], "rotation": [0, 0, 0],
     "params": [8, 0, 2, 0, 4, 0, 5, 6, 0, 0, 0, 0]},
    {"info": 0, "minPlayers": 1, "position": [10, 0, 100], "rotation": [0, 0, 0],
     "params": [9, 0, 2, 0, 4, 0, 6, 0, 0, 0, 0, 0]}
  ],
  "locators": []
})");
    return dir;
}

/** The archive lending the level its torch: TORCHB itself, two frames of its flame, and
 * the flame the braziers' particles wear. */
inline std::filesystem::path sampleLender(std::string_view name) {
    const auto dir = scratchDirectory(name);
    std::filesystem::create_directories(dir / "textures");
    writeFile(dir / "textures/000_TORCHB.png", kTinyPng);
    writeFile(dir / "textures/001_TORCH00.png", kTinyPng);
    writeFile(dir / "textures/002_TORCH01.png", kTinyPng);
    writeFile(dir / "textures/003_P_TORCH.png", kTinyPng);
    writeTextFile(dir / "textures.json", R"({
  "source": "/lender/",
  "defs": [],
  "bitmaps": [
    {"index": 0, "name": "TORCHB", "file": "textures/000_TORCHB.png", "width": 2, "height": 2,
     "format": 50, "flags": 128, "halfResolution": false, "frames": 2},
    {"index": 1, "name": "TORCH00", "file": "textures/001_TORCH00.png", "width": 2, "height": 2,
     "format": 50, "flags": 128, "halfResolution": false, "frames": 0},
    {"index": 2, "name": "TORCH01", "file": "textures/002_TORCH01.png", "width": 2, "height": 2,
     "format": 50, "flags": 128, "halfResolution": false, "frames": 0},
    {"index": 3, "name": "P_TORCH", "file": "textures/003_P_TORCH.png", "width": 2, "height": 2,
     "format": 50, "flags": 128, "halfResolution": false, "frames": 0}
  ]
})");
    return dir;
}

} // namespace gdl::test
