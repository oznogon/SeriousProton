#pragma once

#include "box2d/id.h"
#include <glm/vec2.hpp>

namespace sp
{
namespace multiplayer { class TransformReplication; class PhysicsReplication; }

class CollisionSystem;

// Transform component, to give an entity a position and rotation in the world.
class Transform
{
public:
    glm::vec2 getPosition() const { return position; }
    float getRotation() const { return rotation; }

    void setPosition(glm::vec2 v)
    {
        if (v == position) return;
        position = v;
        position_user_set = true;
        multiplayer_dirty = true;
    }

    void setRotation(float angle)
    {
        if (angle == rotation) return;
        rotation = angle;
        rotation_user_set = true;
        multiplayer_dirty = true;
    }

    // Only use the NoReplication version if the client simulates the same movement.
    void setPositionNoReplication(glm::vec2 v)
    {
        position = v;
        position_user_set = true;
    }

    void setRotationNoReplication(float angle)
    {
        rotation = angle;
        rotation_user_set = true;
    }
private:
    bool position_user_set = false;
    bool rotation_user_set = false;
    bool multiplayer_dirty = false;

    glm::vec2 position{};
    float rotation = 0.0f;

    float last_send_time = 0.0f;
    glm::vec2 last_send_position{};
    float last_send_rotation = 0.0f;

    friend class sp::CollisionSystem;
    friend class sp::multiplayer::TransformReplication;
};

// The physics component gives the entity a physical presence in the physics
// simulation. This includes collision feedback. A physics component does
// nothing on its own, it also needs a Transform component, which is updated by
// the physics system on each physics step. Updating the Transform component
// from another location forces the Physics component to move the object to that
// position.
class Physics
{
public:
    enum class Type
    {
        Sensor,
        Dynamic,
        Static,
    };

    enum class Shape
    {
        Circle,
        Rectangle,
    };

    Type getType() const { return type; }

    void setType(Type type)
    {
        if (type == this->type) return;

        this->type = type;
        physics_dirty = true;
        multiplayer_dirty = true;
    }

    void setCircle(Type type, float radius)
    {
        if (type == this->type
            && shape == Shape::Circle
            && size.x == radius
        ) return;

        this->type = type;
        shape = Shape::Circle;
        size.x = radius;
        size.y = radius;
        physics_dirty = true;
        multiplayer_dirty = true;
    }

    void setRectangle(Type type, glm::vec2 new_size)
    {
        if (type == this->type
            && shape == Shape::Rectangle
            && size == new_size
        ) return;
        
        this->type = type;
        shape = Shape::Rectangle;
        size = new_size;
        physics_dirty = true;
        multiplayer_dirty = true;
    }

    Shape getShape() const { return shape; }
    glm::vec2 getSize() const { return size; }

    glm::vec2 getVelocity() const { return linear_velocity; }
    float getAngularVelocity() const { return angular_velocity; }
    void setVelocity(glm::vec2 velocity)
    {
        linear_velocity_user_set = true;
        linear_velocity = velocity;
    }
    
    void setAngularVelocity(float velocity)
    {
        angular_velocity_user_set = true;
        angular_velocity = velocity;
    }
private:
    bool physics_dirty = true;
    bool multiplayer_dirty = false;

    Type type = Type::Sensor;
    Shape shape = Shape::Circle;
    glm::vec2 size{1.0f, 1.0f};

    b2BodyId body = b2_nullBodyId;
    glm::vec2 linear_velocity{};
    float angular_velocity = 0.0f;
    bool linear_velocity_user_set = false;
    bool angular_velocity_user_set = false;

    friend class sp::CollisionSystem;
    friend class sp::multiplayer::PhysicsReplication;
};
}