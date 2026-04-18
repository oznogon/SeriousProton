#include "multiplayer/collision.h"
#include "ecs/query.h"
#include "ecs/multiplayer.h"
#include "components/collision.h"
#include "engine.h"

#include <glm/geometric.hpp>
#include <cmath>


namespace sp::multiplayer {

void TransformReplication::onEntityDestroyed(uint32_t index)
{
    info.remove(index);
}

void TransformReplication::sendAll(sp::io::DataBuffer& packet)
{
    for(auto [entity, transform] : sp::ecs::Query<sp::Transform>())
    {
        packet << CMD_ECS_SET_COMPONENT << component_index << entity.getIndex();
        auto p = transform.getPosition();
        auto r = transform.getRotation();
        packet << p.x << p.y << r;
    }
}

void TransformReplication::update(sp::io::DataBuffer& packet)
{
    {
        batch_buffer.clear();
        uint16_t delete_count = 0;

        for(auto [index, data] : info) {
            if (!sp::ecs::Entity::forced(index, data).hasComponent<sp::Transform>()) {
                info.remove(index);
                batch_buffer << index;
                delete_count++;
            }
        }

        if (delete_count == 1) {
            packet << CMD_ECS_DEL_COMPONENT << component_index;
            packet.write(batch_buffer);
        } else if (delete_count > 1) {
            packet << CMD_ECS_DEL_COMPONENT_BATCH << component_index << delete_count;
            packet.write(batch_buffer);
        }
    }

    batch_buffer.clear();
    uint16_t update_count = 0;
    auto now = engine->getElapsedTime();

    for(auto [entity, transform] : sp::ecs::Query<sp::Transform>()) {
        if (!info.has(entity.getIndex()) || transform.multiplayer_dirty) {
            auto p = transform.getPosition();
            auto r = transform.getRotation();
            if (info.has(entity.getIndex())) {
                auto position_delta = glm::length(p - transform.last_send_position);
                auto rotation_delta = std::abs(r - transform.last_send_rotation);
                if (position_delta < 0.5f && rotation_delta < 0.5f)
                    continue;
                auto time_between_updates = 1.0f - position_delta / 200.0f - rotation_delta / 100.0f;
                if (time_between_updates < 0.05f)
                    time_between_updates = 0.05f;
                if (transform.last_send_time + time_between_updates > now)
                    continue;
            }
            update_count++;
            batch_buffer << entity.getIndex() << p.x << p.y << r;

            info.set(entity.getIndex(), entity.getVersion());
            transform.multiplayer_dirty = false;
            transform.last_send_position = p;
            transform.last_send_rotation = r;
            transform.last_send_time = now;
        }
    }

    if (update_count == 1) {
        packet << CMD_ECS_SET_COMPONENT << component_index;
        packet.write(batch_buffer);
    } else if (update_count > 1) {
        packet << CMD_ECS_SET_COMPONENT_BATCH << component_index << update_count;
        packet.write(batch_buffer);
    }
}

void TransformReplication::receive(sp::ecs::Entity entity, sp::io::DataBuffer& packet)
{
    float x, y, r;
    packet >> x >> y >> r;
    auto& t = entity.getOrAddComponent<sp::Transform>();
    t.setPosition({x, y});
    t.setRotation(r);
}

void TransformReplication::remove(sp::ecs::Entity entity)
{
    entity.removeComponent<sp::Transform>();
}

void PhysicsReplication::onEntityDestroyed(uint32_t index)
{
    info.remove(index);
}

void PhysicsReplication::sendAll(sp::io::DataBuffer& packet)
{
    for(auto [entity, physics] : sp::ecs::Query<sp::Physics>())
    {
        packet << CMD_ECS_SET_COMPONENT << component_index << entity.getIndex();
        packet << uint32_t(3) << physics.type << physics.shape << physics.size.x << physics.size.y;
        packet << physics.linear_velocity.x << physics.linear_velocity.y << physics.angular_velocity;
    }
}

void PhysicsReplication::update(sp::io::DataBuffer& packet)
{
    for(auto [index, data] : info) {
        if (!sp::ecs::Entity::forced(index, data.version).hasComponent<sp::Physics>()) {
            info.remove(index);
            packet << CMD_ECS_DEL_COMPONENT << component_index << index;
        }
    }
    for(auto [entity, physics] : sp::ecs::Query<sp::Physics>()) {
        if (!info.has(entity.getIndex()) || physics.multiplayer_dirty) {
            info.set(entity.getIndex(), {entity.getVersion(), physics.linear_velocity, physics.angular_velocity});
            packet << CMD_ECS_SET_COMPONENT << component_index << entity.getIndex();
            packet << uint32_t(3) << physics.type << physics.shape << physics.size.x << physics.size.y;
            packet << physics.linear_velocity.x << physics.linear_velocity.y << physics.angular_velocity;
            physics.multiplayer_dirty = false;
        } else {
            auto& i = info.get(entity.getIndex());
            if (glm::length2(i.velocity - physics.linear_velocity) > 5.0f || glm::length2(i.angular_velocity - physics.angular_velocity) > 5.0f) {
                info.set(entity.getIndex(), {entity.getVersion(), physics.linear_velocity, physics.angular_velocity});
                packet << CMD_ECS_SET_COMPONENT << component_index << entity.getIndex();
                packet << uint32_t(2U) << physics.linear_velocity.x << physics.linear_velocity.y << physics.angular_velocity;
            }
        }
    }
}

void PhysicsReplication::receive(sp::ecs::Entity entity, sp::io::DataBuffer& packet)
{
    auto [flags] = packet.read<uint32_t>();
    auto& p = entity.getOrAddComponent<sp::Physics>();
    if (flags & 1U) {
        auto [type, shape, w, h] = packet.read<sp::Physics::Type, sp::Physics::Shape, float, float>();
        switch(shape) {
        case sp::Physics::Shape::Circle:
            p.setCircle(type, w);
            break;
        case sp::Physics::Shape::Rectangle:
            p.setRectangle(type, {w, h});
            break;
        }
    }
    if (flags & 2U) {
        auto [x, y, a] = packet.read<float, float, float>();
        p.setVelocity({x, y});
        p.setAngularVelocity(a);
    }
}

void PhysicsReplication::remove(sp::ecs::Entity entity)
{
    entity.removeComponent<sp::Transform>();
}

}
