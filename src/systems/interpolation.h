#pragma once

#include <ecs/entity.h>
#include <ecs/system.h>
#include <components/collision.h>
#include <unordered_map>
#include <glm/vec2.hpp>

namespace sp {

class InterpolationSystem : public ecs::System
{
public:
    void update(float delta) override;

    static bool hasInterpolatedState(ecs::Entity e);
    static glm::vec2 getPosition(ecs::Entity e);
    static float getRotation(ecs::Entity e);
    static glm::vec2 getVelocity(ecs::Entity e);

    static void setEnabled(bool enabled);
    static bool isEnabled();
    static void setBlendRate(float rate);
    static float getBlendRate();
    static void setTeleportThreshold(float threshold);
    static float getTeleportThreshold();
    static void setDebugEnabled(bool enabled);
    static bool isDebugEnabled();

private:
    struct RenderState {
        uint32_t version = 0;

        glm::vec2 server_position;
        float server_rotation = 0.0f;
        glm::vec2 server_velocity;
        float server_angular_velocity = 0.0f;
        float server_time = 0.0f;

        glm::vec2 render_position;
        float render_rotation = 0.0f;

        glm::vec2 last_server_position;
        float last_server_rotation = 0.0f;
    };

    static std::unordered_map<uint32_t, RenderState> states;
};

}
