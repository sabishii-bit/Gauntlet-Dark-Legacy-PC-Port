#pragma once

namespace gdl::formats {
struct LegendsMap;
struct LegendsSceneMotion;
} // namespace gdl::formats

namespace gdl::game {

/** Samples the native initial world pose once without advancing or triggering a
 * track. Dynamic collision follows the same hierarchy; static collision remains
 * in its already-authored world space. Reapplying is idempotent. */
void applyDreamcastInitialPose(formats::LegendsMap& map, const formats::LegendsSceneMotion& motion);

} // namespace gdl::game
