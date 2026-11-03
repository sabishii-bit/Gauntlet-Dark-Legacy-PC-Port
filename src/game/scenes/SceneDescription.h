#pragma once
#include <string>
#include <vector>

namespace gdl::game {

class LevelWorld;
class PlayScene;
struct SceneSelection;

/**
 * What get info says of a thing selected in `scene`, played over `world`: its token, name and
 * place, and what else is known of its kind. `meshes` is the world whose models may be decoded
 * to name an object's mesh; null in a shared scene, whose objects are described without one.
 */
std::vector<std::string> describeSelection(const PlayScene& scene, const LevelWorld& world,
                                           const SceneSelection& selection, LevelWorld* meshes);

} // namespace gdl::game
