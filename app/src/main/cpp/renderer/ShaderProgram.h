#pragma once

#include <GLES3/gl3.h>
#include <string>
#include <unordered_map>

class ShaderProgram {
public:
    ShaderProgram() = default;
    ~ShaderProgram();

    bool build(const char* vertSrc, const char* fragSrc);
    void use() const;
    void destroy();

    void setUniform1i(const std::string& name, int v)                const;
    void setUniform1f(const std::string& name, float v)              const;
    void setUniform2f(const std::string& name, float x, float y)     const;
    void setUniform3f(const std::string& name, float x, float y, float z) const;
    void setUniform4f(const std::string& name, float x, float y, float z, float w) const;
    void setUniformMat4(const std::string& name, const float* m)     const;

    GLuint programId() const { return m_program; }

private:
    GLuint compileShader(GLenum type, const char* src) const;
    GLint  uniformLocation(const std::string& name)    const;

    GLuint m_program = 0;
    mutable std::unordered_map<std::string, GLint> m_uniformCache;
};
