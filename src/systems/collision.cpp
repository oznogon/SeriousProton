#include "systems/collision.h"
#include "components/collision.h"
#include "ecs/query.h"
#include "engine.h"
#include "random.h"

#include <glm/trigonometric.hpp>
#include <glm/geometric.hpp>

#include <unordered_set>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsuggest-override"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif//__GNUC__
#include "box2d/box2d.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif//__GNUC__

#define BOX2D_SCALE 20.0f

static inline glm::vec2 b2v(b2Vec2 v)
{
    return glm::vec2(v.x * BOX2D_SCALE, v.y * BOX2D_SCALE);
}

static inline b2Vec2 v2b(glm::vec2 v)
{
    return b2Vec2{v.x / BOX2D_SCALE, v.y / BOX2D_SCALE};
}

static b2WorldId worldId = b2_nullWorldId;

namespace sp
{
std::vector<CollisionHandler*> CollisionSystem::handlers;

namespace
{
struct Collision
{
    sp::ecs::Entity A;
    sp::ecs::Entity B;
    float collision_force;
};
}

void CollisionSystem::update(float delta)
{
    if (!B2_IS_NON_NULL(worldId))
    {
        b2WorldDef worldDef = b2DefaultWorldDef();
        worldDef.gravity = b2Vec2{0, 0};
        worldId = b2CreateWorld(&worldDef);
    }
    // If we're paused, don't bother.
    if (delta <= 0.0f) return;

    for (auto [entity, transform, physics] : sp::ecs::Query<Transform, Physics>())
    {
        if (physics.physics_dirty)
        {
            physics.physics_dirty = false;
            sp::ecs::Entity* ptr;

            if (B2_IS_NON_NULL(physics.body))
            {
                ptr = (sp::ecs::Entity*)b2Body_GetUserData(physics.body);
                b2DestroyBody(physics.body);
            }
            else
            {
                ptr = new sp::ecs::Entity();
                *ptr = entity;
            }

            b2BodyDef bodyDef = b2DefaultBodyDef();
            bodyDef.type = physics.type == Physics::Type::Static ? b2_kinematicBody : b2_dynamicBody;
            bodyDef.userData = ptr;
            bodyDef.enableSleep = false;
            bodyDef.position = v2b(transform.position);
            bodyDef.rotation = b2MakeRot(glm::radians(transform.rotation));
            physics.body = b2CreateBody(worldId, &bodyDef);

            b2ShapeDef shapeDef = b2DefaultShapeDef();
            shapeDef.density = 1.f;
            shapeDef.material.friction = 0.f;
            shapeDef.isSensor = physics.type == Physics::Type::Sensor;
            shapeDef.enableSensorEvents = true;
            shapeDef.enableContactEvents = true;

            if (physics.shape == Physics::Shape::Circle)
            {
                b2Circle circle;
                circle.center = b2Vec2{0, 0};
                circle.radius = physics.size.x / BOX2D_SCALE;
                b2CreateCircleShape(physics.body, &shapeDef, &circle);
            }
            else
            {
                b2Polygon box = b2MakeBox(physics.size.x / 2.0f / BOX2D_SCALE, physics.size.y / 2.0f / BOX2D_SCALE);
                b2CreatePolygonShape(physics.body, &shapeDef, &box);
            }
        }

        if (!B2_IS_NON_NULL(physics.body)) continue;

        if (transform.position_user_set)
        {
            b2Body_SetTransform(physics.body, v2b(transform.position), b2Body_GetRotation(physics.body));
            transform.position_user_set = false;
        }

        if (transform.rotation_user_set)
        {
            b2Body_SetTransform(physics.body, b2Body_GetPosition(physics.body), b2MakeRot(glm::radians(transform.rotation)));
            transform.rotation_user_set = false;
        }

        if (physics.linear_velocity_user_set)
        {
            b2Body_SetLinearVelocity(physics.body, v2b(physics.linear_velocity));
            physics.linear_velocity_user_set = false;
        }

        if (physics.angular_velocity_user_set)
        {
            b2Body_SetAngularVelocity(physics.body, glm::radians(physics.angular_velocity));
            physics.angular_velocity_user_set = false;
        }
    }

    b2World_Step(worldId, delta, 4);

    auto now = engine->getElapsedTime();
    std::vector<b2BodyId> remove_list;
    for (auto [entity, transform, physics] : sp::ecs::Query<Transform, Physics>())
    {
        if (!B2_IS_NON_NULL(physics.body))
        {
            physics.physics_dirty = true;
            continue;
        }

        sp::ecs::Entity* entity_ptr = (sp::ecs::Entity*)b2Body_GetUserData(physics.body);
        Transform* transform_ptr;
        Physics* physics_ptr = nullptr;

        if (!*entity_ptr
            || !(physics_ptr = entity_ptr->getComponent<Physics>())
            || !(transform_ptr = entity_ptr->getComponent<Transform>())
        ) {
            delete entity_ptr;
            remove_list.push_back(physics.body);
            physics.body = b2_nullBodyId;
            physics.physics_dirty = true;
        }
        else
        {
            transform_ptr->position = b2v(b2Body_GetPosition(physics.body));
            transform_ptr->rotation = glm::degrees(b2Rot_GetAngle(b2Body_GetRotation(physics.body)));
            physics_ptr->linear_velocity = b2v(b2Body_GetLinearVelocity(physics.body));
            physics_ptr->angular_velocity = glm::degrees(b2Body_GetAngularVelocity(physics.body));

            auto position_delta = glm::length(transform_ptr->position - transform_ptr->last_send_position);
            auto rotation_delta = std::abs(transform_ptr->rotation - transform_ptr->last_send_rotation);
            if (position_delta < 0.5f && rotation_delta < 0.5f) continue;

            auto time_between_updates = 1.0f - position_delta / 200.0f - rotation_delta / 100.0f;
            if (time_between_updates < 0.05f) time_between_updates = 0.05f;

            if (transform_ptr->last_send_time + time_between_updates < now)
                transform_ptr->multiplayer_dirty = true;
        }
    }

    for (auto body : remove_list) b2DestroyBody(body);

    std::vector<Collision> collision_pair_list;
    std::unordered_set<uint64_t> processed_pairs;
    for (auto [entity, transform, physics] : sp::ecs::Query<Transform, Physics>())
    {
        if (!B2_IS_NON_NULL(physics.body)) continue;

        if (physics.type == Physics::Type::Sensor)
        {
            // Box2D v3 limits simultaneous overlapping contacts to 16.
            b2ShapeId shapes[8];
            int shape_count = b2Body_GetShapes(physics.body, shapes, 8);
            b2ShapeId overlaps[16];

            for (int s = 0; s < shape_count && s < 8; s++)
            {
                int overlap_cap = b2Shape_GetSensorCapacity(shapes[s]);
                int overlap_count = b2Shape_GetSensorOverlaps(shapes[s], overlaps, overlap_cap < 16 ? overlap_cap : 16);

                for (int o = 0; o < overlap_count && o < 16; o++)
                {
                    if (!b2Shape_IsValid(overlaps[o])) continue;

                    b2BodyId otherBody = b2Shape_GetBody(overlaps[o]);
                    sp::ecs::Entity* other_ptr = (sp::ecs::Entity*)b2Body_GetUserData(otherBody);
                    if (!other_ptr || !*other_ptr) continue;

                    uint32_t a = entity.getIndex();
                    uint32_t b = other_ptr->getIndex();
                    if (a == b) continue;

                    if (a > b) std::swap(a, b);

                    uint64_t pair_key = (uint64_t(a) << 32) | uint64_t(b);

                    if (processed_pairs.find(pair_key) != processed_pairs.end())
                        continue;

                    processed_pairs.insert(pair_key);
                    collision_pair_list.push_back({entity, *other_ptr, 0.0f});
                }
            }
        }
        else
        {
            // Box2D v3 limits simultaneous overlapping contacts to 16.
            b2ContactData contacts[16];
            int count = b2Body_GetContactData(physics.body, contacts, 16);
            if (count > 16) count = 16;

            for (int i = 0; i < count; i++)
            {
                b2BodyId bodyA = b2Shape_GetBody(contacts[i].shapeIdA);
                b2BodyId bodyB = b2Shape_GetBody(contacts[i].shapeIdB);

                bool this_is_a = B2_ID_EQUALS(physics.body, bodyA);
                b2BodyId otherBody = this_is_a ? bodyB : bodyA;

                sp::ecs::Entity* other_ptr = (sp::ecs::Entity*)b2Body_GetUserData(otherBody);
                if (!other_ptr || !*other_ptr) continue;

                uint32_t a = entity.getIndex();
                uint32_t b = other_ptr->getIndex();
                if (a == b) continue;

                if (a > b) std::swap(a, b);

                uint64_t pair_key = (uint64_t(a) << 32) | uint64_t(b);

                if (processed_pairs.find(pair_key) != processed_pairs.end())
                    continue;

                processed_pairs.insert(pair_key);

                // Apply force to colliders.
                float force = 0.0f;
                for (int n = 0; n < contacts[i].manifold.pointCount; n++)
                    force += contacts[i].manifold.points[n].normalImpulse * BOX2D_SCALE;

                collision_pair_list.push_back({entity, *other_ptr, force});
            }
        }
    }

    for (auto& pair : collision_pair_list)
    {
        if (!pair.A || !pair.B) continue;

        for (auto handler : handlers)
        {
            if (!pair.A || !pair.B) break;
            handler->collision(pair.A, pair.B, pair.collision_force);

            if (!pair.A || !pair.B) break;
            handler->collision(pair.B, pair.A, pair.collision_force);
        }
    }
}

static bool queryCallback(b2ShapeId shapeId, void* context)
{
    auto list = (std::vector<sp::ecs::Entity>*)context;
    b2BodyId bodyId = b2Shape_GetBody(shapeId);
    auto ptr = (sp::ecs::Entity*)b2Body_GetUserData(bodyId);
    if (ptr && *ptr) list->push_back(*ptr);

    return true;
}

std::vector<sp::ecs::Entity> CollisionSystem::queryArea(glm::vec2 lowerBound, glm::vec2 upperBound)
{
    std::vector<sp::ecs::Entity> list;
    b2AABB aabb;
    aabb.lowerBound = v2b(lowerBound);
    aabb.upperBound = v2b(upperBound);

    if (aabb.lowerBound.x > aabb.upperBound.x)
        std::swap(aabb.upperBound.x, aabb.lowerBound.x);

    if (aabb.lowerBound.y > aabb.upperBound.y)
        std::swap(aabb.upperBound.y, aabb.lowerBound.y);

    if (B2_IS_NON_NULL(worldId))
        b2World_OverlapAABB(worldId, aabb, b2DefaultQueryFilter(), queryCallback, &list);

    return list;
}

std::vector<sp::ecs::Entity> TransformQuery::queryArea(glm::vec2 lowerBound, glm::vec2 upperBound)
{
    std::vector<sp::ecs::Entity> result;

    for (auto [entity, transform, physics] : sp::ecs::Query<Transform, sp::ecs::optional<Physics>>())
    {
        auto radius = physics ? physics->getSize().x : 0;

        if (transform.getPosition().x + radius < lowerBound.x
            || transform.getPosition().x - radius > upperBound.x
        ) continue;

        if (transform.getPosition().y + radius < lowerBound.y
            || transform.getPosition().y - radius > upperBound.y
        ) continue;

        result.push_back(entity);
    }

    return result;
}
}
