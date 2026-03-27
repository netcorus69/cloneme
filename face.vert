#version 330 core

layout(location = 0) in vec3 aPos;        // base position
layout(location = 1) in vec3 aNormal;     // base normal
layout(location = 4) in vec3 aMorphMouth; // morph target 0
layout(location = 5) in vec3 aMorphEyelid;// morph target 1
layout(location = 6) in vec3 aMorphPupil; // morph target 2

uniform float uMorph0; // mouth weight
uniform float uMorph1; // eyelid weight
uniform float uMorph2; // pupil weight

uniform mat4 MVP;
uniform mat4 M;

out vec3 vNormal;

void main() {
    // Blend base position with morphs
    vec3 pos = aPos 
             + uMorph0 * aMorphMouth
             + uMorph1 * aMorphEyelid
             + uMorph2 * aMorphPupil;

    gl_Position = MVP * vec4(pos, 1.0);

    // Blend normals similarly if needed
    vNormal = aNormal; // (extend if morph normals available)
}
