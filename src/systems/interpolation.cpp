#include "systems/interpolation.h"
#include "ecs/query.h"
#include "engine.h"
#include "vectorUtils.h"
#include "multiplayer_client.h"

#include <cmath>

namespace sp {

std::unordered_map<uint32_t, InterpolationSystem::RenderState> InterpolationSystem::states;

static float interpolation_blend_rate = 10.0f;
static bool  interpolation_enabled    = true;
static float teleport_snap_threshold  = 500.0f;
static bool  interpolation_debug      = false;

void InterpolationSystem::setEnabled(bool enabled) { interpolation_enabled = enabled; }
bool InterpolationSystem::isEnabled() { return interpolation_enabled; }
void InterpolationSystem::setBlendRate(float rate) { interpolation_blend_rate = rate; }
float InterpolationSystem::getBlendRate() { return interpolation_blend_rate; }
void InterpolationSystem::setTeleportThreshold(float threshold) { teleport_snap_threshold = threshold; }
float InterpolationSystem::getTeleportThreshold() { return teleport_snap_threshold; }
void InterpolationSystem::setDebugEnabled(bool enabled) { interpolation_debug = enabled; }
bool InterpolationSystem::isDebugEnabled() { return interpolation_debug; }

void InterpolationSystem::update(float delta)
{
    if (!game_client || !interpolation_enabled)
        return;

    float now = engine->getElapsedTime();

    // Remove states for destroyed or transformed entities
    for (auto it = states.begin(); it != states.end(); ) {
        auto e = ecs::Entity::forced(it->first, it->second.version);
        if (!e || !e.hasComponent<Transform>())
            it = states.erase(it);
        else
            ++it;
    }

    for (auto [entity, transform] : ecs::Query<Transform>()) {
        auto index = entity.getIndex();
        auto version = entity.getVersion();
        auto it = states.find(index);
        bool is_new = (it == states.end());

        glm::vec2 current_pos = transform.getPosition();
        float current_rot = transform.getRotation();

        if (is_new) {
            RenderState state;
            state.version = version;
            state.server_position = current_pos;
            state.server_rotation = current_rot;
            state.server_time = now;
            state.render_position = current_pos;
            state.render_rotation = current_rot;
            state.last_server_position = current_pos;
            state.last_server_rotation = current_rot;

            if (auto physics = entity.getComponent<Physics>()) {
                state.server_velocity = physics->getVelocity();
                state.server_angular_velocity = physics->getAngularVelocity();
            }

            states.emplace(index, state);
            continue;
        }

        auto& state = it->second;
        state.version = version;

        // Detect authoritative position/rotation changes by comparing against
        // the last known server state. Use epsilon to avoid false positives
        // from floating-point imprecision in locally computed positions.
        bool server_updated = (glm::length2(current_pos - state.last_server_position) > 0.0001f * 0.0001f)
                           || (std::abs(current_rot - state.last_server_rotation) > 0.0001f);

        if (server_updated) {
            // Check for teleport (jump drive, wormhole) and snap instantly
            if (glm::length2(current_pos - state.last_server_position) > teleport_snap_threshold * teleport_snap_threshold) {
                state.render_position = current_pos;
                state.render_rotation = current_rot;
            }

            state.server_position = current_pos;
            state.server_rotation = current_rot;
            state.server_time = now;
            state.last_server_position = current_pos;
            state.last_server_rotation = current_rot;

            if (auto physics = entity.getComponent<Physics>()) {
                state.server_velocity = physics->getVelocity();
                state.server_angular_velocity = physics->getAngularVelocity();
            }
        }

        // Dead reckoning: predict where the server thinks the object is now.
        // Clamp extrapolation to 0.5s to prevent unbounded drift during
        // long network gaps (e.g. packet loss or stalls).
        float dt = std::min(now - state.server_time, 0.5f);
        glm::vec2 predicted_pos = state.server_position + state.server_velocity * dt;
        float predicted_rot = state.server_rotation + state.server_angular_velocity * dt;

        // Exponential smoothing toward the predicted state
        float blend = 1.0f - std::exp(-interpolation_blend_rate * delta);

        glm::vec2 error_pos = predicted_pos - state.render_position;
        state.render_position += error_pos * blend;

        float error_rot = angleDifference(state.render_rotation, predicted_rot);
        state.render_rotation += error_rot * blend;
    }
}

bool InterpolationSystem::hasInterpolatedState(ecs::Entity e)
{
    return states.find(e.getIndex()) != states.end();
}

glm::vec2 InterpolationSystem::getPosition(ecs::Entity e)
{
    auto it = states.find(e.getIndex());
    if (it != states.end())
        return it->second.render_position;
    if (auto transform = e.getComponent<Transform>())
        return transform->getPosition();
    return {};
}

float InterpolationSystem::getRotation(ecs::Entity e)
{
    auto it = states.find(e.getIndex());
    if (it != states.end())
        return it->second.render_rotation;
    if (auto transform = e.getComponent<Transform>())
        return transform->getRotation();
    return 0.0f;
}

glm::vec2 InterpolationSystem::getVelocity(ecs::Entity e)
{
    auto it = states.find(e.getIndex());
    if (it != states.end())
        return it->second.server_velocity;
    if (auto physics = e.getComponent<Physics>())
        return physics->getVelocity();
    return {};
}

}
