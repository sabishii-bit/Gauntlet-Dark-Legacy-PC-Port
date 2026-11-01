#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/render/Mesh.h"

#include "formats/AnimationTree.h"

namespace gdl::formats {

struct LegendsMapCollisionTriangle;

enum class LegendsN64FixtureKind { Powerup, Decoration, Generator, Trap, Teleport, Container };

struct LegendsN64Fixture {
    LegendsN64FixtureKind kind = LegendsN64FixtureKind::Powerup;
    u32 nativeIndex = 0;
    u16 templateKey = 0;
    u16 resolvedTemplateKey = 0; ///< initial selector result; original placement key is retained
    u8 minimumPlayers = 0;
    u8 placementFlags = 0;
    u32 templateFlags = 0;
    u32 resolvedTemplateFlags = 0; ///< selected template, including native pickup grant ID
    s16 templateAmount = 0;        ///< PPower quantity; timed grants express seconds
    u32 pickupLimit = 0;           ///< native PPower inventory cap, not a GDL limit
    u32 pickupSound = 0;           ///< native message/sound cue; never a duration
    f32 pickupRadius = 0.5f;
    f32 pickupHeight = 3.0f;
    Vec3 position{0.0f};
    f32 facingRadians = 0.0f;
    f32 scale = 1.0f;
    std::string group;
    std::string sequence;
    std::string activeSequence;
    std::string deathSequence;
    std::string archive; ///< optional native per-pickup library, relative to extraction root
    bool groundProjected = true;
    bool actorInsteadOfGenerator = false;
    u8 generatorTier = 0;
    u16 containedPowerup = 0;
    s8 quantity = 0;
};

/** Generic placement template ROMs, not renderer object archives. */
struct LegendsN64Templates {
    std::span<const u8> powerups;
    std::span<const u8> decorations;
    std::span<const u8> generators;
    std::span<const u8> traps;
    std::span<const u8> teleports;
    std::span<const u8> containers;
    std::span<const u8> randoms;
};

std::vector<LegendsN64Fixture> decodeLegendsN64Fixtures(std::span<const u8> bytes,
                                                        const LegendsN64Templates& templates,
                                                        u8 activationThreshold = 1);

/** Native slot order, shared by BODY references and object-frame animation. */
struct LegendsN64ColorRamp {
    u32 part = 0;
    Color primitive = Color::white();
    Color environment = Color::white();
    bool shadeRgb = false; ///< Second RDP cycle modulates RGB by SHADE; alpha stays TEXEL0.a.
    bool operator==(const LegendsN64ColorRamp&) const = default;
};

struct LegendsN64Model {
    // Segment13 is supplied by a drawable's property7, not by a local texture index.
    static constexpr u32 kRuntimeTexture = 0xffffffffU;
    std::string name;
    u32 flags = 0;
    Mesh mesh;
    /** Original 16-byte Vtx index for each decoded mesh vertex. Cache uploads
     * may duplicate vertices; native morph bindings address this original pool. */
    std::vector<u32> vertexSources;
    std::string partName; ///< OBHD+6 suffix used by the native recursive attachment lookup
    /** Authored (ENV-PRIM)*TEXEL0+PRIM, alpha=TEXEL0.a. These parts do not use SHADE. */
    std::vector<LegendsN64ColorRamp> colorRamps;
    /** Uniform texture-only models' authored Z_CMP/Z_UPD bits, translated to
     * object_flags. Shaded/inherited/mixed part state remains outside this mapping. */
    std::optional<u32> depthFlags;
    bool mixedDepthFlags = false;
};
std::vector<LegendsN64Model> decodeLegendsN64Models(std::span<const u8> bytes);

struct LegendsN64Sequences {
    AnimationFile animation;
    /** Node-specific segment13 bindings. Append after native model slots; their
     * unique names are referenced by the decoded trees. Unbound original parts
     * with kRuntimeTexture must not be submitted to a renderer. */
    std::vector<LegendsN64Model> modelVariants;
    std::vector<std::string> warnings;
};

/** One tree per native group/sequence pair, named group + ':' + FOURCC.
 * Optional dynseq.rom supplies the archive's externally streamed SLST records.
 * Grouped combatants may bake per-sequence drawable texture assignments into
 * bounded object runs; ordinary fixtures retain their shared/keyed animation form.
 * Unsupported native tracks are reported, never reinterpreted as GameCube tracks. */
LegendsN64Sequences decodeLegendsN64Sequences(std::span<const u8> bytes,
                                              std::span<const LegendsN64Model> models,
                                              std::span<const u8> program = {},
                                              std::span<const u8> dynamicSequences = {},
                                              bool bakeSequenceTextures = false);

std::string legendsN64TreeName(std::string_view group, std::string_view sequence);

/** Native item animation archives in load order, relative to the extracted ROM root.
 * Unknown level IDs return no guessed realm. items/generic is a texture-only adjunct;
 * each returned path supplies its own objects.wad and seq.wad. */
std::vector<std::string> legendsN64FixtureArchives(std::string_view levelName,
                                                   std::span<const LegendsN64Fixture> fixtures = {},
                                                   u8 activationThreshold = 1);

/** Initial placement only: native floor masks/search windows and 0.2-unit surface pad.
 * No floor match leaves the authored position intact; mounted traps never project. */
struct LegendsN64FixturePlacement {
    Vec3 position{0};
    s32 floorObject = -1;
};
LegendsN64FixturePlacement
legendsN64FixturePlacement(const LegendsN64Fixture& fixture,
                           std::span<const LegendsMapCollisionTriangle> collision);
Vec3 legendsN64FixturePosition(const LegendsN64Fixture& fixture,
                               std::span<const LegendsMapCollisionTriangle> collision);

} // namespace gdl::formats
