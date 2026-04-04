/**
 * MMDRenderer.cpp
 * OpenGL ES 3.0 renderer for PMX models via Saba.
 *
 * Model (Yvonne.pmx) coordinate analysis:
 *   - 97.7% of vertices in Y range 0..25 (height ~20 units)
 *   - Center approximately (0, 10, 0)
 *   - Camera: eye=(0,13,30), target=(0,10,0), FOV=30 fits the full figure
 */

#include "MMDRenderer.h"

#include <android/log.h>
#include <GLES3/gl3.h>

#include <Saba/Model/MMD/PMXModel.h>
#include <Saba/Model/MMD/MMDModel.h>
#include <Saba/Model/MMD/MMDMaterial.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define LOG_TAG "MMDRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── Shaders ──────────────────────────────────────────────────────────────────

static const char* TOON_VERT = R"GLSL(#version 300 es
precision highp float;
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_norm;
layout(location=2) in vec2 a_uv;
out vec3 v_norm; out vec2 v_uv; out vec3 v_world;
uniform mat4 u_mvp; uniform mat4 u_model;
uniform vec2 u_offset; uniform float u_scale;
void main(){
    v_world = (u_model * vec4(a_pos,1)).xyz;
    v_norm  = normalize(mat3(transpose(inverse(u_model)))*a_norm);
    v_uv    = a_uv;
    vec4 c  = u_mvp * vec4(a_pos,1);
    c.x += u_offset.x*c.w; c.y += u_offset.y*c.w;
    c.xyz *= u_scale; gl_Position = c;
}
)GLSL";

static const char* TOON_FRAG = R"GLSL(#version 300 es
precision highp float;
in vec3 v_norm; in vec2 v_uv; in vec3 v_world;
out vec4 fragColor;
uniform sampler2D u_tex; uniform int u_hasTex;
uniform vec4 u_diff;        // rgb=diffuse color, a=material alpha
uniform vec3 u_spec;
uniform float u_specPow;
uniform vec3 u_amb;
uniform float u_alpha;

const vec3 LIGHT_DIR = normalize(vec3(0.5, 1.0, 0.7));
const vec3 LIGHT_COL = vec3(1.0, 0.98, 0.95);
// Ambient light color — simulates MMD's default ambient setting
const vec3 AMB_LIGHT  = vec3(0.6, 0.6, 0.6);

void main(){
    // Base color from texture if present
    vec4 texColor = vec4(1.0);
    if(u_hasTex == 1) texColor = texture(u_tex, v_uv);

    // MMD standard lighting model:
    //   diffuse_contribution  = material_diffuse * texture * lightDiffuse * NdotL_toon
    //   ambient_contribution  = material_ambient * texture * lightAmbient
    //   total = diffuse + ambient (no double-add)
    vec3 N = normalize(v_norm);
    float NdotL = max(dot(N, LIGHT_DIR), 0.0);

    // Soft toon step — gentler than before, closer to MMD default
    float toon = NdotL > 0.6 ? 1.0 : mix(0.5, 1.0, NdotL / 0.6);

    // Diffuse: material_diffuse * texture_rgb * light_color * toon
    vec3 diffuse = u_diff.rgb * texColor.rgb * LIGHT_COL * toon;

    // Ambient: material_ambient * texture_rgb * ambient_light
    vec3 ambient = u_amb * texColor.rgb * AMB_LIGHT;

    // Specular (optional, keep subtle)
    vec3 viewDir = normalize(-v_world);
    vec3 halfDir = normalize(LIGHT_DIR + viewDir);
    float s = pow(max(dot(N, halfDir), 0.0), max(u_specPow, 1.0));
    vec3 specular = u_spec * s * 0.3 * toon;

    // Final alpha: material alpha * texture alpha * global alpha
    float finalA = u_diff.a * texColor.a * u_alpha;
    if(finalA < 0.01) discard;

    fragColor = vec4(diffuse + ambient + specular, finalA);
}
)GLSL";

static const char* OUTLINE_VERT = R"GLSL(#version 300 es
precision highp float;
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_norm;
uniform mat4 u_mvp; uniform float u_width;
uniform vec2 u_offset; uniform float u_scale;
void main(){
    vec4 c = u_mvp * vec4(a_pos + a_norm*u_width, 1);
    c.x+=u_offset.x*c.w; c.y+=u_offset.y*c.w;
    c.xyz*=u_scale; gl_Position=c;
}
)GLSL";

static const char* OUTLINE_FRAG = R"GLSL(#version 300 es
precision mediump float;
out vec4 fragColor;
uniform vec4 u_color; uniform float u_alpha;
void main(){ fragColor=vec4(u_color.rgb, u_color.a*u_alpha); }
)GLSL";

// ─── Helpers ──────────────────────────────────────────────────────────────────

static std::string fixPath(const std::string& p){
    std::string r=p; std::replace(r.begin(),r.end(),'\\','/'); return r;
}

static unsigned char* loadImg(const std::string& path, int*w, int*h, int*c){
    unsigned char* d = stbi_load(path.c_str(),w,h,c,4);
    if(d) return d;
    static const char* ext[]=
        {".png",".PNG",".tga",".TGA",".bmp",".BMP",".jpg",".JPG",nullptr};
    auto dot = path.rfind('.');
    if(dot==std::string::npos) return nullptr;
    std::string base = path.substr(0,dot);
    for(int i=0;ext[i];++i){
        std::string alt=base+ext[i];
        if(alt==path) continue;
        d=stbi_load(alt.c_str(),w,h,c,4);
        if(d){ LOGI("Tex fallback: %s",alt.c_str()); return d; }
    }
    return nullptr;
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

MMDRenderer::MMDRenderer()  = default;
MMDRenderer::~MMDRenderer() { shutdown(); }

bool MMDRenderer::initialize(int width, int height){
    m_width=width; m_height=height;
    m_toonShader    = std::make_unique<ShaderProgram>();
    m_outlineShader = std::make_unique<ShaderProgram>();
    if(!m_toonShader->build(TOON_VERT,TOON_FRAG)){LOGE("toon shader fail");return false;}
    if(!m_outlineShader->build(OUTLINE_VERT,OUTLINE_FRAG)){LOGE("outline fail");return false;}
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0,0,0,0);
    LOGI("initialize OK (%dx%d)",width,height);
    return true;
}

void MMDRenderer::shutdown(){
    for(auto t:m_textures) if(t) glDeleteTextures(1,&t);
    m_textures.clear();
    if(m_vboPos)  {glDeleteBuffers(1,&m_vboPos); m_vboPos=0;}
    if(m_vboNorm) {glDeleteBuffers(1,&m_vboNorm);m_vboNorm=0;}
    if(m_vboUV)   {glDeleteBuffers(1,&m_vboUV);  m_vboUV=0;}
    if(m_ibo)     {glDeleteBuffers(1,&m_ibo);     m_ibo=0;}
    if(m_vao)     {glDeleteVertexArrays(1,&m_vao);m_vao=0;}
    if(m_toonShader)    m_toonShader->destroy();
    if(m_outlineShader) m_outlineShader->destroy();
    m_model.reset(); m_modelLoaded=false;
    LOGI("shutdown");
}

// ─── Model load ───────────────────────────────────────────────────────────────

bool MMDRenderer::loadPMXModel(const std::string& pmxPath){
    LOGI("loadPMXModel: %s", pmxPath.c_str());
    if(m_modelLoaded){
        for(auto t:m_textures) glDeleteTextures(1,&t); m_textures.clear();
        if(m_vboPos)  {glDeleteBuffers(1,&m_vboPos); m_vboPos=0;}
        if(m_vboNorm) {glDeleteBuffers(1,&m_vboNorm);m_vboNorm=0;}
        if(m_vboUV)   {glDeleteBuffers(1,&m_vboUV);  m_vboUV=0;}
        if(m_ibo)     {glDeleteBuffers(1,&m_ibo);     m_ibo=0;}
        if(m_vao)     {glDeleteVertexArrays(1,&m_vao);m_vao=0;}
        m_model.reset(); m_modelLoaded=false;
    }

    m_model = std::make_unique<saba::PMXModel>();
    std::string dir = pmxPath.substr(0, pmxPath.find_last_of("/\\"));
    if(!m_model->Load(pmxPath, dir)){
        LOGE("PMXModel::Load failed"); m_model.reset(); return false;
    }

    // ── Initialize and run one full animation cycle for bind pose ─────────
    // This fills GetUpdatePositions() with valid data BEFORE VMDManager::update()
    // is ever called, so the model is visible on the very first frame.
    m_model->InitializeAnimation();
    m_model->BeginAnimation();
    m_model->UpdateAllAnimation(nullptr, 0.f, 0.f);
    m_model->Update();      // ← fills m_updatePositions (skin mesh)
    m_model->EndAnimation();

    buildVAO();       // uploads GetUpdatePositions() → now has valid data
    loadTextures();

    m_modelLoaded = true;
    LOGI("PMX OK: verts=%zu mats=%zu subMeshes=%zu",
         m_model->GetVertexCount(),
         m_model->GetMaterialCount(),
         m_model->GetSubMeshCount());
    return true;
}

// ─── VAO ──────────────────────────────────────────────────────────────────────

void MMDRenderer::buildVAO(){
    if(!m_model) return;
    size_t vc = m_model->GetVertexCount();
    size_t ic = m_model->GetIndexCount();
    size_t is = m_model->GetIndexElementSize();
    const void* ri = m_model->GetIndices();

    glGenVertexArrays(1,&m_vao); glBindVertexArray(m_vao);

    // Positions — use GetUpdatePositions() (already filled by initial Update())
    glGenBuffers(1,&m_vboPos); glBindBuffer(GL_ARRAY_BUFFER,m_vboPos);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(vc*sizeof(glm::vec3)),
                 m_model->GetUpdatePositions(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(glm::vec3),nullptr);

    // Normals
    glGenBuffers(1,&m_vboNorm); glBindBuffer(GL_ARRAY_BUFFER,m_vboNorm);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(vc*sizeof(glm::vec3)),
                 m_model->GetUpdateNormals(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(glm::vec3),nullptr);

    // UVs (static)
    glGenBuffers(1,&m_vboUV); glBindBuffer(GL_ARRAY_BUFFER,m_vboUV);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(vc*sizeof(glm::vec2)),
                 m_model->GetUVs(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,sizeof(glm::vec2),nullptr);

    // Indices → expand to uint32
    glGenBuffers(1,&m_ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,m_ibo);
    if(is==4){
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)(ic*4),ri,GL_STATIC_DRAW);
    } else {
        std::vector<uint32_t> ex(ic);
        if(is==1){ auto s=(const uint8_t*)ri;  for(size_t i=0;i<ic;++i) ex[i]=s[i]; }
        else      { auto s=(const uint16_t*)ri; for(size_t i=0;i<ic;++i) ex[i]=s[i]; }
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)(ic*4),ex.data(),GL_STATIC_DRAW);
    }
    glBindVertexArray(0);
}

// ─── Textures ─────────────────────────────────────────────────────────────────

void MMDRenderer::loadTextures(){
    if(!m_model) return;
    size_t mc = m_model->GetMaterialCount();
    m_textures.assign(mc,0);
    int ok=0, fail=0;
    for(size_t i=0;i<mc;++i){
        const auto& mat = m_model->GetMaterials()[i];
        if(mat.m_texture.empty()) continue;
        std::string p = fixPath(mat.m_texture);
        LOGI("Texture[%zu]: %s", i, p.c_str());
        int w=0,h=0,c=0;
        unsigned char* d = loadImg(p,&w,&h,&c);
        if(!d){
            LOGE("Texture FAILED[%zu]: %s — %s", i, p.c_str(), stbi_failure_reason());
            fail++; continue;
        }
        glGenTextures(1,&m_textures[i]);
        glBindTexture(GL_TEXTURE_2D,m_textures[i]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,d);
        glGenerateMipmap(GL_TEXTURE_2D);
        stbi_image_free(d); ok++;
    }
    LOGI("Textures: %d loaded, %d failed / %zu mats", ok, fail, mc);
}

// ─── Per-frame ────────────────────────────────────────────────────────────────

void MMDRenderer::uploadVertices(){
    if(!m_model) return;
    size_t n = m_model->GetVertexCount();
    glBindBuffer(GL_ARRAY_BUFFER,m_vboPos);
    glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)(n*12),m_model->GetUpdatePositions());
    glBindBuffer(GL_ARRAY_BUFFER,m_vboNorm);
    glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)(n*12),m_model->GetUpdateNormals());
}

void MMDRenderer::onSurfaceChanged(int w, int h){
    m_width=w; m_height=h; glViewport(0,0,w,h);
    LOGI("onSurfaceChanged %dx%d",w,h);
}
void MMDRenderer::onTouchDown(float x, float y){ LOGI("onTouchDown (%.1f, %.1f)",x,y); }
void MMDRenderer::setTransform(float x,float y,float s,float a){
    m_posX=x; m_posY=y; m_scale=s; m_alpha=a;
}
void MMDRenderer::setMorphWeight(const std::string& name, float w){
    if(!m_model||!m_modelLoaded) return;
    auto* mgr = m_model->GetMorphManager();
    auto* m   = mgr ? mgr->GetMorph(name) : nullptr;
    if(m) m->SetWeight(w);
}

// ─── Render ───────────────────────────────────────────────────────────────────

void MMDRenderer::render(float /*dt*/){
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    if(!m_modelLoaded||!m_model) return;

    uploadVertices();

    // Camera calibrated for Yvonne.pmx:
    // Model: Y=0..20 (height~20 units), center=(0,10,0)
    // At Z=42, FOV=28: half-height = 42*tan(14°) = 42*0.249 = 10.5u
    // → full 21u visible with small margin, full model fits in frame
    float aspect = m_height>0
        ? static_cast<float>(m_width)/static_cast<float>(m_height) : 1.f;
    glm::mat4 proj  = glm::perspective(glm::radians(28.f), aspect, 0.1f, 500.f);
    glm::mat4 view  = glm::lookAt(glm::vec3(0, 10, 42),
                                  glm::vec3(0, 10,  0),
                                  glm::vec3(0,  1,  0));
    glm::mat4 model = glm::mat4(1.f);
    glm::mat4 mvp   = proj * view * model;

    // Outline — always cull back (we expand normals outward)
    glCullFace(GL_FRONT); glEnable(GL_CULL_FACE);
    m_outlineShader->use();
    m_outlineShader->setUniformMat4("u_mvp",   glm::value_ptr(mvp));
    m_outlineShader->setUniform1f  ("u_width",  0.012f);
    m_outlineShader->setUniform2f  ("u_offset", m_posX, m_posY);
    m_outlineShader->setUniform1f  ("u_scale",  m_scale);
    m_outlineShader->setUniform4f  ("u_color",  0.05f,0.05f,0.05f,1.f);
    m_outlineShader->setUniform1f  ("u_alpha",  m_alpha);
    drawOutline();

    // Toon — face culling handled per-material (m_bothFace)
    m_toonShader->use();
    m_toonShader->setUniformMat4("u_mvp",   glm::value_ptr(mvp));
    m_toonShader->setUniformMat4("u_model", glm::value_ptr(model));
    m_toonShader->setUniform2f  ("u_offset", m_posX, m_posY);
    m_toonShader->setUniform1f  ("u_scale",  m_scale);
    m_toonShader->setUniform1f  ("u_alpha",  m_alpha);
    drawModel();
    glDisable(GL_CULL_FACE);  // reset after all materials
}

void MMDRenderer::drawModel(){
    if(!m_vao||!m_model) return;
    glBindVertexArray(m_vao);
    const saba::MMDMaterial* mats = m_model->GetMaterials();
    const saba::MMDSubMesh*  sms  = m_model->GetSubMeshes();
    size_t sc = m_model->GetSubMeshCount();
    for(size_t i=0;i<sc;++i){
        const saba::MMDSubMesh&  sm  = sms[i];
        const saba::MMDMaterial& mat = mats[sm.m_materialID];
        m_toonShader->setUniform4f("u_diff",
            mat.m_diffuse.r, mat.m_diffuse.g, mat.m_diffuse.b, mat.m_alpha);
        m_toonShader->setUniform3f("u_spec",
            mat.m_specular.r, mat.m_specular.g, mat.m_specular.b);
        m_toonShader->setUniform1f("u_specPow", mat.m_specularPower);
        m_toonShader->setUniform3f("u_amb",
            mat.m_ambient.r, mat.m_ambient.g, mat.m_ambient.b);

        // Per-material face culling: bothFace=true means hair/cloth visible from both sides
        if (mat.m_bothFace) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
        }

        size_t mi = (size_t)sm.m_materialID;
        GLuint tid = mi<m_textures.size() ? m_textures[mi] : 0;
        m_toonShader->setUniform1i("u_hasTex", tid?1:0);
        if(tid){ glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tid);
                 m_toonShader->setUniform1i("u_tex",0); }
        glDrawElements(GL_TRIANGLES,(GLsizei)sm.m_vertexCount,GL_UNSIGNED_INT,
                       (void*)((uintptr_t)sm.m_beginIndex*4));
    }
    glBindVertexArray(0);
}

void MMDRenderer::drawOutline(){
    if(!m_vao||!m_model) return;
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES,(GLsizei)m_model->GetIndexCount(),GL_UNSIGNED_INT,nullptr);
    glBindVertexArray(0);
}
