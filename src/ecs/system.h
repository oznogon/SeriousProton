#pragma once

namespace sp::ecs {

class System {
public:
    virtual ~System() = default;
    virtual void update(float delta) = 0;
};

}