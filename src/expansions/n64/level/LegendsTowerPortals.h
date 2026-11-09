#pragma once

#include <span>
#include <string_view>

#include "engine/render/SceneFog.h"

#include "expansions/shared/level/LegendsPortalVisitor.h"
namespace gdl::game {

enum class LegendsTowerPortalKind { Shards, Runestones };

/** e08641c4's cathedral departure fog in native projected-depth space. */
SceneFog legendsCathedralFog(f32 seconds);
struct LegendsTowerPortalDefinition {
    Vec3 position{0};
    f32 radius = 0;
    std::string_view map;
};

/** Tower.bin's scripted completion exits are not IPR transporter fixtures.
 * Their contact test uses player centres in XZ, without body radii or height.
 * This class emits travel once; player departure and presentation belong to
 * the scene. Call with simulation time, not interpolated presentation time. */
class LegendsTowerPortalMotion {
public:
    explicit LegendsTowerPortalMotion(LegendsTowerPortalKind kind);
    void capturePresentation();
    bool advance(f32 seconds, std::span<const LegendsPortalVisitor> visitors, bool enabled);

    LegendsTowerPortalKind kind() const { return m_kind; }
    const LegendsTowerPortalDefinition& definition() const { return m_definition; }
    f32 charge(f32 alpha = -1) const;
    /** e0864914 creates BOSSDEATHFX on first contact, then raises/lowers
     * its tree at 80 world units/second between -40 and the circle's floor. */
    f32 fireHeight(f32 alpha = -1) const;
    bool fireCreated() const { return m_fireCreated; }
    f32 dwell() const { return static_cast<f32>(m_dwell); }
    u8 occupied() const { return m_occupied; }
    bool departed() const { return m_departed; }

private:
    LegendsTowerPortalKind m_kind;
    LegendsTowerPortalDefinition m_definition;
    f64 m_charge = 0;
    f64 m_previousCharge = 0;
    f64 m_dwell = 0;
    f64 m_fireHeight = -40;
    f64 m_previousFireHeight = -40;
    u8 m_occupied = 0;
    bool m_departed = false;
    bool m_fireCreated = false;
};

} // namespace gdl::game
