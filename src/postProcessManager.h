#pragma once

#include "graphics/shader.h"
#include "graphics/renderTexture.h"
#include "stringImproved.h"
#include "Updatable.h"
#include "Renderable.h"
#include <glm/vec2.hpp>

class PostProcessor : public RenderChain
{
private:
    sp::Shader* shader;
    sp::RenderTexture render_texture;

    RenderChain* chain;
    std::unordered_map<string, float> uniforms;
    std::unordered_map<string, glm::vec2> vec2_uniforms;

    unsigned int vertices_vbo = 0;
    unsigned int indices_vbo = 0;
public:
    bool enabled;

    PostProcessor(string shadername, RenderChain* chain);
    virtual ~PostProcessor() {}

    virtual void render(sp::RenderTarget& target) override;

    virtual bool onPointerMove(glm::vec2 position, sp::io::Pointer::ID id) override;
    virtual void onPointerLeave(sp::io::Pointer::ID id) override;
    virtual bool onPointerDown(sp::io::Pointer::Button button, glm::vec2 position, sp::io::Pointer::ID id) override;
    virtual void onPointerDrag(glm::vec2 position, sp::io::Pointer::ID id) override;
    virtual void onMouseWheelScroll(glm::vec2 position, float value) override;
    virtual void onPointerUp(glm::vec2 position, sp::io::Pointer::ID id) override;
    virtual void onTextInput(const string& text) override;
    virtual void onTextInput(sp::TextInputEvent e) override;

    void setUniform(string name, float value);
    void setUniform(string name, glm::vec2 value);
};
