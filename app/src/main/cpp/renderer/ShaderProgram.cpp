/**
 * ShaderProgram.cpp
 * Compile, link, and manage a GLSL ES 3.0 shader program.
 */

#include "ShaderProgram.h"
#include <android/log.h>
#include <vector>

#define LOG_TAG "ShaderProgram"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

ShaderProgram::~ShaderProgram() { destroy(); }

bool ShaderProgram::build(const char* vertSrc, const char* fragSrc) {
    GLuint vert = compileShader(GL_VERTEX_SHADER,   vertSrc);
    GLuint frag = compileShader(GL_FRAGMENT_SHADER, fragSrc);
    if (!vert || !frag) return false;

    m_program = glCreateProgram();
    glAttachShader(m_program, vert);
    glAttachShader(m_program, frag);
    glLinkProgram(m_program);
    glDeleteShader(vert);
    glDeleteShader(frag);

    GLint status = GL_FALSE;
    glGetProgramiv(m_program, GL_LINK_STATUS, &status);
    if (status == GL_FALSE) {
        GLint logLen = 0;
        glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log(static_cast<size_t>(logLen + 1));
        glGetProgramInfoLog(m_program, logLen, nullptr, log.data());
        LOGE("Program link error:\n%s", log.data());
        glDeleteProgram(m_program);
        m_program = 0;
        return false;
    }

    LOGI("Shader program built  id=%u", m_program);
    return true;
}

void ShaderProgram::use() const { glUseProgram(m_program); }

void ShaderProgram::destroy() {
    if (m_program) {
        glDeleteProgram(m_program);
        m_program = 0;
        m_uniformCache.clear();
    }
}

GLuint ShaderProgram::compileShader(GLenum type, const char* src) const {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok == GL_FALSE) {
        GLint logLen = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log(static_cast<size_t>(logLen + 1));
        glGetShaderInfoLog(sh, logLen, nullptr, log.data());
        LOGE("Shader compile error (type=%u):\n%s", type, log.data());
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLint ShaderProgram::uniformLocation(const std::string& name) const {
    auto it = m_uniformCache.find(name);
    if (it != m_uniformCache.end()) return it->second;
    GLint loc = glGetUniformLocation(m_program, name.c_str());
    m_uniformCache[name] = loc;
    return loc;
}

void ShaderProgram::setUniform1i(const std::string& n, int v) const
    { glUniform1i(uniformLocation(n), v); }
void ShaderProgram::setUniform1f(const std::string& n, float v) const
    { glUniform1f(uniformLocation(n), v); }
void ShaderProgram::setUniform2f(const std::string& n, float x, float y) const
    { glUniform2f(uniformLocation(n), x, y); }
void ShaderProgram::setUniform3f(const std::string& n, float x, float y, float z) const
    { glUniform3f(uniformLocation(n), x, y, z); }
void ShaderProgram::setUniform4f(const std::string& n, float x, float y, float z, float w) const
    { glUniform4f(uniformLocation(n), x, y, z, w); }
void ShaderProgram::setUniformMat4(const std::string& n, const float* m) const
    { glUniformMatrix4fv(uniformLocation(n), 1, GL_FALSE, m); }
