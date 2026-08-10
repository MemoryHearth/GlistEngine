#version 450

// 3D mesh shader for the Vulkan backend. Deliberately separate from the OpenGL
// color_vert.glsl: that one reads its matrices from default-block uniforms, which
// Vulkan/SPIR-V forbids, and carries shadow/fog/instancing machinery this path does
// not have yet. The lighting maths in the fragment stage is ported from
// color_frag.glsl so both backends shade a surface the same way.
//
// Only the attributes this stage reads are declared. The pipeline describes the
// gVertex layout by hand (see gVKPipeline.cpp) and hands over just the entries the
// shader actually consumes, so the locations here line up with that description.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
// The bitangent attribute at location 4 is deliberately not read: it is derived
// from the normal and the tangent below, exactly as color_vert.glsl does.
layout(location = 3) in vec3 aTangent;
layout(location = 5) in vec3 aColor;

// Per instance rather than per vertex: it comes from a second vertex binding whose
// input rate is INSTANCE, which is Vulkan's equivalent of glVertexAttribDivisor(1).
// A mat4 occupies four consecutive locations, so this claims 6 through 9.
//
// pc.misc.y carries the instancing bit as well as the diffuse-map bit. Keeping the
// attribute in the common layout lets both draw forms share a pipeline, while the
// uniform branch avoids a mat4*mat4 for every vertex of an ordinary mesh.
layout(location = 6) in mat4 aInstanceModel;

// Everything that is the same for every mesh in a frame. Matches gVKSceneUniforms
// in gVKUniform.h field for field.
struct Light {
    int type;
    vec3 position;
    vec3 direction;
    vec4 ambient;
    vec4 diffuse;
    vec4 specular;
    float constant;
    float linear;
    float quadratic;
    float cutOff;
    float outerCutOff;
};

layout(set = 0, binding = 0) uniform Scene {
    mat4 projection;
    mat4 view;
    mat4 lightmatrix;
    vec4 viewpos;
    vec4 globalambientcolor;
    vec4 rendercolor;
    // xyz the shadow-casting light's position, w whether a shadow map is bound.
    vec4 shadowlightpos;
    int lightnum;
    int enabledlights;
    int softshadows;
    Light lights[8];
} scene;

// Per mesh. Held to 128 bytes, the smallest push constant range Vulkan guarantees,
// so this works on every conforming implementation rather than only on desktop
// drivers that happen to offer 256.
layout(push_constant) uniform Push {
    mat4 model;
    vec4 ambient;
    vec4 diffuse;
    vec4 specular;
    // x shininess; y diffuse-map bit + 2*instancing; z specular map; w normal map.
    vec4 misc;
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vFragPos;
layout(location = 2) out vec3 vColor;
layout(location = 3) out vec2 vTexCoords;
// Tangent space, for normal mapping. A mat3 takes three locations, so these run
// 4..6 and the two vectors follow. They are only computed when a normal map is
// present, matching the OpenGL path and avoiding substantial work on dense maps.
layout(location = 4) out mat3 vTBN;
layout(location = 7) out vec3 vTangentViewPos;
layout(location = 8) out vec3 vTangentFragPos;
// Where this fragment falls in the shadow map, still homogeneous - the perspective
// divide happens in the fragment stage so it is done per pixel rather than
// interpolated, which is what color_vert.glsl does with FragPosLightSpace.
layout(location = 9) out vec4 vFragPosLightSpace;

void main() {
    bool instanced = pc.misc.y >= 2.0;
    mat4 model = instanced ? pc.model * aInstanceModel : pc.model;

    vec4 world = model * vec4(aPos, 1.0);
    // The projection already carries the Vulkan depth correction; see
    // gVKRenderEngine::setProjectionMatrix.
    gl_Position = scene.projection * scene.view * world;

    vFragPos = world.xyz;
    // Normals do not survive a non-uniform scale under the model matrix itself, so
    // the inverse-transpose is what keeps them perpendicular to the surface. The
    // OpenGL path builds this on the CPU; here it is cheaper to derive than to
    // spend another 48 bytes of the push constant budget carrying it.
    mat3 normalmatrix = transpose(inverse(mat3(model)));
    vNormal = normalmatrix * aNormal;
    vColor = aColor;
    vTexCoords = aTexCoords;

    if (pc.misc.w > 0.0) {
        // Gram-Schmidt: the tangent is re-orthogonalised against the normal, and
        // the bitangent comes from their cross product rather than the attribute.
        vec3 T = normalize(normalmatrix * aTangent);
        vec3 N = normalize(vNormal);
        T = normalize(T - dot(T, N) * N);
        vec3 B = cross(N, T);
        vTBN = transpose(mat3(T, B, N));
        vTangentViewPos = vTBN * scene.viewpos.xyz;
        vTangentFragPos = vTBN * vFragPos;
    }

    vFragPosLightSpace = scene.lightmatrix * vec4(vFragPos, 1.0);
}
