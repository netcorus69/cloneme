// face.c
// ─────────────────────────────────────────────────────────────────────────────
// CloneMe — 3D head renderer using SDL2 + OpenGL 3.3 Core
// Runs as a forked child process to keep rendering independent from main app.
// Loads a .obj head mesh, applies a texture, and renders it in a window.
// Mouth animation is controlled via a shared file: /tmp/cloneme_mouth
// ─────────────────────────────────────────────────────────────────────────────

#include "face.h"
#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <locale.h>
#include <semaphore.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

// stb_image: single-header image loader (PNG, JPG, etc.)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ─── GLOBALS ──────────────────────────────────────────────────────────────────

static pid_t face_pid = -1;     // PID of the forked renderer process
int mouth_open = 0;             // 0 = closed, 1 = open (set by main process)
sem_t face_ready_sem;           // semaphore to signal when face process is ready

// GPU handles for the head mesh
static GLuint g_head_vao = 0;       // Vertex Array Object for head geometry
static int    g_head_idx_count = 0; // Number of indices to draw

static GLuint g_albedo_tex = 0;     // Texture ID for the face/skin texture

// Debug toggle: 0 = textured shader, 1 = normal visualization shader
static int g_normal_vis = 0;

// ─── SHADERS ──────────────────────────────────────────────────────────────────
// Shaders are written in GLSL 330 (OpenGL 3.3 Core)

// VERTEX SHADER
// Transforms each vertex from object space to clip space using MVP matrix.
// Passes normals, world position, and UV coords to the fragment shader.
static const char *vert_src =
"#version 330 core\n"
"layout(location=0) in vec3 aPos;\n"
"layout(location=1) in vec3 aNorm;\n"
"layout(location=2) in vec2 aUV;\n"
"out vec3 vN; out vec3 vP; out vec2 vUV;\n"
"uniform mat4 MVP; uniform mat4 M;\n"
"uniform float uMouth;\n"
"void main(){\n"
"  vec3 pos = aPos;\n"
// Top lip — just above the bottom lip range
"  if(pos.y > 0.5 && pos.y < 0.9){\n"
"    float w = (pos.y - 0.5) / 0.4;\n"
"    pos.y -= uMouth * 0.8 * w;\n"   // moves opposite to bottom
"  }\n"
// Bottom lip — keep exactly what works
"  if(pos.y > 0.1 && pos.y < 0.5){\n"
"    float w = 1.0 - (pos.y - 0.1) / 0.4;\n"
"    pos.y += uMouth * 0.5 * w;\n"
"  }\n"
"  vP = vec3(M * vec4(pos,1.0));\n"
"  vN = normalize(mat3(M) * aNorm);\n"
"  vUV = aUV;\n"
"  gl_Position = MVP * vec4(pos,1.0);\n"
"}\n";

// FRAGMENT SHADER — Normal visualization (debug)
// Colors the mesh based on surface normals: useful to check mesh orientation.
// Red = X axis, Green = Y axis, Blue = Z axis
static const char *frag_normal_src =
"#version 330 core\n"
"in vec3 vN; in vec3 vP; in vec2 vUV;\n"
"out vec4 C;\n"
"void main(){\n"
"  vec3 N = normalize(vN);\n"
"  vec3 Nvis = N * 0.5 + 0.5;\n"  // remap -1..1 to 0..1 for color display
"  C = vec4(Nvis, 1.0);\n"
"}\n";

// FRAGMENT SHADER — Final textured shader
// Samples the albedo (skin) texture and outputs it directly.
// NOTE: multiply t.rgb by value > 1.0 to brighten (was 3.0, now 1.0 = natural)
static const char *frag_final_src =
"#version 330 core\n"
"in vec3 vN; in vec3 vP; in vec2 vUV;\n"
"out vec4 C;\n"
"uniform sampler2D albedoTex;\n"
"void main(){\n"
"  vec3 N = normalize(vN);\n"
// Light coming from front-top (like a face lamp)
"  vec3 L = normalize(vec3(0.3, 1.0, 2.0));\n"
// Diffuse: how much surface faces the light
"  float diff = max(dot(N, L), 0.0);\n"
// Ambient: minimum brightness so shadows aren't black
"  float ambient = 0.4;\n"
"  float light = ambient + diff * 0.7;\n"
"  vec4 t = texture(albedoTex, vUV);\n"
"  C = vec4(t.rgb * light, 1.0);\n"
"}\n";

static const char *frag_src = NULL;  // points to whichever frag shader is active

// ─── MATH ─────────────────────────────────────────────────────────────────────
// Simple column-major 4x4 float matrix math (no external library needed)

typedef float M4[16];

// Identity matrix
static void mid(M4 m){ memset(m,0,64); m[0]=m[5]=m[10]=m[15]=1; }

// Matrix multiply: r = a * b
static void mmul(M4 r, const M4 a, const M4 b){
    M4 t; memset(t,0,64);
    for(int i=0;i<4;i++) for(int j=0;j<4;j++) for(int k=0;k<4;k++)
        t[j*4+i] += a[k*4+i] * b[j*4+k];
    memcpy(r,t,64);
}

// Perspective projection matrix
// fv = field of view (radians), asp = aspect ratio, n/f = near/far planes
static void mpersp(M4 m, float fv, float asp, float n, float f){
    memset(m,0,64); float t=tanf(fv*.5f);
    m[0]=1/(asp*t); m[5]=1/t; m[10]=(f+n)/(n-f);
    m[14]=2*f*n/(n-f); m[11]=-1;
}

// Translation matrix: moves object to (x,y,z)
static void mtrans(M4 m, float x, float y, float z){
    mid(m); m[12]=x; m[13]=y; m[14]=z;
}

// Scale matrix: scales object by (x,y,z)
static void mscl(M4 m, float x, float y, float z){
    mid(m); m[0]=x; m[5]=y; m[10]=z;
}

// ─── SPHERE (fallback geometry) ───────────────────────────────────────────────
// A basic UV sphere used as fallback if the .obj file fails to load.

#define SL 32   // longitude slices
#define ST 20   // latitude stacks
static GLuint g_vao; static int g_ic;

static void init_sphere(){
    int vc=(SL+1)*(ST+1);
    float *v=malloc(vc*6*4); int p=0;
    // Generate sphere vertices: position + normal (same for unit sphere)
    for(int i=0;i<=ST;i++){
        float phi=(float)M_PI*i/ST-(float)M_PI/2;
        for(int j=0;j<=SL;j++){
            float th=2*(float)M_PI*j/SL;
            float nx=cosf(phi)*cosf(th), ny=sinf(phi), nz=cosf(phi)*sinf(th);
            v[p++]=nx; v[p++]=ny; v[p++]=nz;  // position
            v[p++]=nx; v[p++]=ny; v[p++]=nz;  // normal
        }
    }
    // Generate triangle indices (two triangles per quad)
    g_ic=ST*SL*6; unsigned *idx=malloc(g_ic*4); p=0;
    for(int i=0;i<ST;i++) for(int j=0;j<SL;j++){
        int a=i*(SL+1)+j, b=a+SL+1;
        idx[p++]=a; idx[p++]=b; idx[p++]=a+1;
        idx[p++]=b; idx[p++]=b+1; idx[p++]=a+1;
    }
    // Upload to GPU
    GLuint vbo,ebo;
    glGenVertexArrays(1,&g_vao); glGenBuffers(1,&vbo); glGenBuffers(1,&ebo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,vc*24,v,GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,g_ic*4,idx,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,24,(void*)0);  glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,24,(void*)12); glEnableVertexAttribArray(1);
    free(v); free(idx);
}

// ─── SHADER PROGRAM ───────────────────────────────────────────────────────────

static GLuint g_prog;   // active shader program ID
static M4 g_vp;         // combined View * Projection matrix (shared across draws)

// Draw sphere at given position/scale/color (debug or fallback)
static void dp(float tx, float ty, float tz,
               float sx, float sy, float sz,
               float r,  float g,  float b, float sp){
    M4 M,S,T,MVP;
    mtrans(T,tx,ty,tz); mscl(S,sx,sy,sz); mmul(M,T,S); mmul(MVP,g_vp,M);
    glUniformMatrix4fv(glGetUniformLocation(g_prog,"MVP"),1,0,MVP);
    glUniformMatrix4fv(glGetUniformLocation(g_prog,"M"),1,0,M);
    glUniform3f(glGetUniformLocation(g_prog,"col"),r,g,b);
    glUniform1f(glGetUniformLocation(g_prog,"sp"),sp);
    glBindVertexArray(g_vao);
    glDrawElements(GL_TRIANGLES,g_ic,GL_UNSIGNED_INT,0);
}

// Compile a single shader stage (vertex or fragment)
// Returns shader ID on success, 0 on failure
static GLuint compile_shader(GLenum type, const char *src){
    GLuint id = glCreateShader(type);
    glShaderSource(id,1,&src,NULL);
    glCompileShader(id);
    GLint ok=0; glGetShaderiv(id,GL_COMPILE_STATUS,&ok);
    if(!ok){
        char log[4096]; glGetShaderInfoLog(id,sizeof(log),NULL,log);
        fprintf(stderr,"Shader compile error: %s\n",log);
        glDeleteShader(id); return 0;
    }
    return id;
}

// Link vertex + fragment shaders into a full program
static GLuint mkprog(void){
    if(!frag_src) frag_src = g_normal_vis ? frag_normal_src : frag_final_src;
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if(!vs || !fs){ if(vs) glDeleteShader(vs); if(fs) glDeleteShader(fs); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p,vs); glAttachShader(p,fs);
    glLinkProgram(p);
    GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){
        char log[4096]; glGetProgramInfoLog(p,sizeof(log),NULL,log);
        fprintf(stderr,"[DEBUG] Program link error: %s\n",log);
        glDeleteProgram(p); p=0;
    } else {
        printf("[DEBUG] Shader program linked successfully, ID=%u\n",p);
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

// ─── OBJ LOADER ───────────────────────────────────────────────────────────────
// Loads a Wavefront .obj file into a VAO ready for glDrawElements.
// Supports: v (positions), vt (UVs), vn (normals), f (faces v/vt/vn format)
// Interleaved vertex layout: [px,py,pz, nx,ny,nz, u,v] = 8 floats per vertex

typedef struct { float x,y,z; } V3;
typedef struct { float u,v;   } UV;
typedef struct { float x,y,z; } N3;

// Parse 3 floats using strtod (locale-safe, always uses dot as decimal separator)
static void parse3f(const char *ptr, float *a, float *b, float *c){
    char *e;
    *a = (float)strtod(ptr, &e);
    *b = (float)strtod(e,   &e);
    *c = (float)strtod(e,   NULL);
}

// Parse 2 floats using strtod (locale-safe)
static void parse2f(const char *ptr, float *a, float *b){
    char *e;
    *a = (float)strtod(ptr, &e);
    *b = (float)strtod(e,   NULL);
}

static int load_obj_to_vao(const char *path, GLuint *out_vao, int *out_index_count){
    FILE *f = fopen(path,"r");
    if(!f){ perror("load_obj_to_vao fopen"); fprintf(stderr,"OBJ open failed: %s\n",path); return 0; }

    // Raw data arrays parsed from the .obj file
    V3 *pos=NULL; int pos_n=0, pos_cap=0;
    UV *uv =NULL; int uv_n=0,  uv_cap=0;
    N3 *nor=NULL; int nor_n=0, nor_cap=0;

    // Flattened interleaved vertex data ready for GPU upload
    float    *vdata=NULL; int vdata_n=0, vdata_cap=0;
    unsigned *idx  =NULL; int idx_n=0,   idx_cap=0;

    char line[512];
    while(fgets(line,sizeof(line),f)){

        // "v x y z" — vertex position
        if(line[0]=='v' && line[1]==' '){
            float x,y,z; parse3f(line+2,&x,&y,&z);
            if(pos_n+1>pos_cap){ pos_cap=pos_cap?pos_cap*2:256; pos=realloc(pos,sizeof(V3)*pos_cap); }
            pos[pos_n++]=(V3){x,y,z};
        }

        // "vt u v" — texture coordinate
        // V is flipped (1-v) because OpenGL UV origin is bottom-left,
        // but image files have origin at top-left
        else if(line[0]=='v' && line[1]=='t'){
            float u,v; parse2f(line+3,&u,&v);
            printf("[DEBUG] vt u=%f v=%f\n",u,v);
            if(uv_n+1>uv_cap){ uv_cap=uv_cap?uv_cap*2:256; uv=realloc(uv,sizeof(UV)*uv_cap); }
            uv[uv_n++]=(UV){u, 1.0f-v};  // flip V axis
        }

        // "vn x y z" — vertex normal
        else if(line[0]=='v' && line[1]=='n'){
            float x,y,z; parse3f(line+3,&x,&y,&z);
            if(nor_n+1>nor_cap){ nor_cap=nor_cap?nor_cap*2:256; nor=realloc(nor,sizeof(N3)*nor_cap); }
            nor[nor_n++]=(N3){x,y,z};
        }

        // "f v/vt/vn ..." — triangle face (supports v/vt/vn, v//vn, v/vt)
        else if(line[0]=='f'){
            int vi[3],vti[3],vni[3];
            for(int i=0;i<3;i++) vi[i]=vti[i]=vni[i]=0;

            // Try full format: v/vt/vn
            int matches = sscanf(line+1,"%d/%d/%d %d/%d/%d %d/%d/%d",
                &vi[0],&vti[0],&vni[0], &vi[1],&vti[1],&vni[1], &vi[2],&vti[2],&vni[2]);
            if(matches<9){
                // Try no-UV format: v//vn
                matches = sscanf(line+1,"%d//%d %d//%d %d//%d",
                    &vi[0],&vni[0], &vi[1],&vni[1], &vi[2],&vni[2]);
                if(matches<6){
                    // Try no-normal format: v/vt
                    sscanf(line+1,"%d/%d %d/%d %d/%d",
                        &vi[0],&vti[0], &vi[1],&vti[1], &vi[2],&vti[2]);
                }
            }

            // Unpack each of the 3 triangle vertices
            for(int k=0;k<3;k++){
                int p=vi[k]-1, t=vti[k]-1, n=vni[k]-1;  // .obj is 1-based
                float px=0,py=0,pz=0, nx=0,ny=0,nz=0, u=0,vv=0;
                if(p>=0 && p<pos_n){ px=pos[p].x; py=pos[p].y; pz=pos[p].z; }
                if(t>=0 && t<uv_n ){ u=uv[t].u;   vv=uv[t].v; }
                if(n>=0 && n<nor_n){ nx=nor[n].x;  ny=nor[n].y; nz=nor[n].z; }

                int cur = vdata_n/8;  // index of this vertex

                // Grow vertex buffer if needed
                if(vdata_n+8>vdata_cap){
                    vdata_cap=vdata_cap?vdata_cap*2:4096;
                    vdata=realloc(vdata,sizeof(float)*vdata_cap);
                }
                // Write interleaved: [pos.xyz, normal.xyz, uv.uv]
                vdata[vdata_n++]=px; vdata[vdata_n++]=py; vdata[vdata_n++]=pz;
                vdata[vdata_n++]=nx; vdata[vdata_n++]=ny; vdata[vdata_n++]=nz;
                vdata[vdata_n++]=u;  vdata[vdata_n++]=vv;

                // Grow index buffer if needed
                if(idx_n+1>idx_cap){
                    idx_cap=idx_cap?idx_cap*2:4096;
                    idx=realloc(idx,sizeof(unsigned)*idx_cap);
                }
                idx[idx_n++]=(unsigned)cur;
            }
        }
    }
    fclose(f);
    
    // After the while(fgets...) loop, add:
	printf("[DEBUG] Y range: min=%f max=%f\n", 
    pos[0].y, pos[0].y);  // we'll find actual min/max
	float ymin=99999, ymax=-99999;
	for(int i=0;i<pos_n;i++){
    if(pos[i].y < ymin) ymin=pos[i].y;
    if(pos[i].y > ymax) ymax=pos[i].y;
	}
	printf("[DEBUG] Mesh Y: min=%f max=%f\n", ymin, ymax);
    
    
    if(vdata_n==0){ fprintf(stderr,"OBJ had no vertices: %s\n",path); return 0; }

    // Upload to GPU
    GLuint vao,vbo,ebo;
    glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&vbo); glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof(float)*vdata_n,vdata,GL_STATIC_DRAW);
    glGenBuffers(1,&ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(unsigned)*idx_n,idx,GL_STATIC_DRAW);

    // Define vertex attribute layout (must match vertex shader layout locations)
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(0));               // position
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(3*sizeof(float))); // normal
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(6*sizeof(float))); // UV
    glEnableVertexAttribArray(2);

    fprintf(stderr,"Loaded %d vertices, %d indices from %s\n",vdata_n/8,idx_n,path);
    free(pos); free(uv); free(nor); free(vdata); free(idx);
    *out_vao=vao; *out_index_count=idx_n;
    return 1;
}

// ─── DRAW MESH ────────────────────────────────────────────────────────────────
// Helper: draws any VAO mesh at given position/scale using current shader

static void draw_mesh_with_dp(GLuint vao, int index_count,
                              float tx, float ty, float tz,
                              float sx, float sy, float sz,
                              float r,  float g,  float b, float sp)
{
    if(vao==0 || index_count==0) return;
    M4 M,S,T,MVP;
    mtrans(T,tx,ty,tz); mscl(S,sx,sy,sz); mmul(M,T,S); mmul(MVP,g_vp,M);
    glUniformMatrix4fv(glGetUniformLocation(g_prog,"MVP"),1,0,MVP);
    glUniformMatrix4fv(glGetUniformLocation(g_prog,"M"),1,0,M);
    glUniform3f(glGetUniformLocation(g_prog,"col"),r,g,b);
    glUniform1f(glGetUniformLocation(g_prog,"sp"),sp);
    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES,index_count,GL_UNSIGNED_INT,0);
}

// ─── TEXTURE LOADER ───────────────────────────────────────────────────────────

static GLuint load_texture(const char *path){
    int w,h,n;
    unsigned char *data = stbi_load(path,&w,&h,&n,3);  // force RGB (3 channels)
    if(!data){
        fprintf(stderr,"[DEBUG] Texture load failed for %s: %s\n",path,stbi_failure_reason());
        return 0;
    }
    GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,w,h,0,GL_RGB,GL_UNSIGNED_BYTE,data);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);         // tile horizontally
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);         // tile vertically
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR); // smooth minify
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);     // smooth magnify
    stbi_image_free(data);
    printf("[DEBUG] Loaded texture %s (%dx%d, channels=%d), ID=%u\n",path,w,h,n,tex);
    return tex;
}

// ─── ROTATION MATRICES ────────────────────────────────────────────────────────

// X axis rotation (pitch): + = tilt forward, - = tilt back
static void mrotx(M4 m, float a){
    mid(m); m[5]=cosf(a); m[9]=-sinf(a); m[6]=sinf(a); m[10]=cosf(a);
}

// Y axis rotation (yaw): + = turn right, - = turn left
static void mroty(M4 m, float a){
    mid(m); m[0]=cosf(a); m[8]=sinf(a); m[2]=-sinf(a); m[10]=cosf(a);
}

// Z axis rotation (roll): + = tilt right, - = tilt left
static void mrotz(M4 m, float a){
    mid(m); m[0]=cosf(a); m[4]=-sinf(a); m[1]=sinf(a); m[5]=cosf(a);
}

// ─── RENDER ───────────────────────────────────────────────────────────────────
// Called every frame. Applies transforms and draws the head mesh.
// TODO: when separating mesh into parts (eyes, mouth, etc.),
//       add individual transform blocks here for each part.

static void render(int m, int tick){
    glUseProgram(g_prog);

    // Smooth mouth animation: interpolates between 0.0 (closed) and 1.0 (open)
    static float s_mouth = 0.0f;
    float target = m ? 1.0f : 0.0f;
    s_mouth += (target - s_mouth) * 0.2f;
    glUniform1f(glGetUniformLocation(g_prog, "uMouth"), s_mouth);

    // Set color uniform to white (texture provides actual color)
    glUniform3f(glGetUniformLocation(g_prog,"col"),1.0f,1.0f,1.0f);

    // Bind face texture to texture unit 0
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,g_albedo_tex);

    if(g_head_vao && g_head_idx_count){
        M4 S, Rx, Ry, T, tmp, tmp2, M_mat, MVP;

        // ── Transform pipeline ────────────────────────────────────────────────
        // Tune these values to position/orient the head correctly:
        //   mscl:   uniform scale (0.12 = small, increase to make head bigger)
        //   mrotx:  pitch in radians (0 = no tilt, 1.5708 = 90° forward)
        //   mroty:  yaw in radians   (0 = no turn, 3.1416 = 180° flip)
        //   mtrans: world position   (x=left/right, y=up/down, z=near/far)
        mscl(S,   0.12f, 0.12f, 0.12f);  // scale
        mrotx(Rx, 0.0f);                  // pitch — tune if head tilts up/down
        mroty(Ry, 0.0f);                  // yaw   — tune if head faces wrong way
        mtrans(T, 0.0f, 0.0f, -1.0f);    // position (T, 0.0f, 0.0f, -1.0f)

        // Build model matrix: Scale * Rotation * Translation
        mmul(tmp,   Ry,   Rx);     // combined rotation
        mmul(tmp2,  S,    tmp);    // scale applied to rotation
        mmul(M_mat, T,    tmp2);   // final model matrix
        mmul(MVP,   g_vp, M_mat);  // MVP = projection * view * model

        // Upload matrices to shader
        glUniformMatrix4fv(glGetUniformLocation(g_prog,"MVP"),1,0,MVP);
        glUniformMatrix4fv(glGetUniformLocation(g_prog,"M"),1,0,M_mat);

        // Draw head
        glBindVertexArray(g_head_vao);
        glDrawElements(GL_TRIANGLES,g_head_idx_count,GL_UNSIGNED_INT,0);
    }
}

// ─── FACE PROCESS ─────────────────────────────────────────────────────────────
// Main loop for the renderer child process.
// Initializes SDL2 + OpenGL, loads assets, renders at ~30fps.

static void run_face_process(){
    printf("[DEBUG] Initializing SDL...\n");

    SDL_Init(SDL_INIT_VIDEO);

    // Request OpenGL 3.3 Core profile
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS,1);  // enable MSAA
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES,4);  // 4x MSAA

    SDL_Window *win = SDL_CreateWindow("CloneMe",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        400, 450, SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN);

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    glewExperimental = GL_TRUE; glewInit();

    glEnable(GL_DEPTH_TEST);   // correct 3D depth ordering
    glEnable(GL_MULTISAMPLE);  // MSAA anti-aliasing
    printf("[DEBUG] SDL window and GL context created.\n");

    // IMPORTANT: SDL_Init resets the locale to system default.
    // Force "C" locale so strtod always uses dot as decimal separator.
    // Without this, OBJ parsing breaks on systems using comma decimals (e.g. Italian locale).
    setlocale(LC_NUMERIC,"C");
    printf("[DEBUG] Locale set to C (dot decimal)\n");

    // Compile and link shader program
    frag_src = g_normal_vis ? frag_normal_src : frag_final_src;
    g_prog = mkprog();
    if(!g_prog) fprintf(stderr,"[DEBUG] Warning: shader program creation failed\n");

    // Init fallback sphere (shown if OBJ fails to load)
    init_sphere();
    glUseProgram(g_prog);

    // Load head mesh
    // TODO: add more load calls here for eyes.obj, mouth.obj, eyebrows.obj, etc.
    if(!load_obj_to_vao("monkey.obj",&g_head_vao,&g_head_idx_count)){
        fprintf(stderr,"[DEBUG] Failed to load head.obj — using sphere fallback\n");
    } else {
        printf("[DEBUG] Head mesh loaded: VAO=%u, indices=%d\n",g_head_vao,g_head_idx_count);
    }

    // Load face/skin texture
    // TODO: load per-part textures when mesh is split into parts
    g_albedo_tex = load_texture("monkey.png");
    glUseProgram(g_prog);
    glUniform1i(glGetUniformLocation(g_prog,"albedoTex"),0);  // texture unit 0
    printf("[DEBUG] Albedo texture ID=%u\n",g_albedo_tex);

    // Fallback: 1x1 white texture if face.png fails to load
    if(g_albedo_tex==0){
        unsigned char white[3]={255,255,255};
        glGenTextures(1,&g_albedo_tex);
        glBindTexture(GL_TEXTURE_2D,g_albedo_tex);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,1,1,0,GL_RGB,GL_UNSIGNED_BYTE,white);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        fprintf(stderr,"[DEBUG] Created 1x1 white fallback texture\n");
    }

    // Build View * Projection matrix (static — camera doesn't move)
    // view: slight Y offset + push back on Z axis
    // proj: ~30° FOV, window aspect ratio, near=0.1, far=50
    M4 view,proj;
    mtrans(view,0,.10f,-4.5f);
    mpersp(proj,.52f,400.f/450.f,.1f,50.f);
    mmul(g_vp,proj,view);

    // ── Main render loop ──────────────────────────────────────────────────────
    int m=0, tick=0;
    SDL_Event e;
    while(1){
        // Handle quit event (window X button)
        while(SDL_PollEvent(&e)) if(e.type==SDL_QUIT) goto done;

        // Read mouth state from shared temp file (written by main process)
        // 0 = mouth closed, 1 = mouth open
        FILE *f = fopen("/tmp/cloneme_mouth","r");
        if(f){ if(fscanf(f,"%d",&m)!=1) m=0; fclose(f); }

        // Clear screen and render frame
        glClearColor(.22f,.22f,.28f,1.f);  // dark blue-grey background
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        render(m,tick);
        SDL_GL_SwapWindow(win);
        SDL_Delay(33);  // ~30 fps
        tick++;
    }
done:
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    exit(0);
}

// ─── PUBLIC API ───────────────────────────────────────────────────────────────
// These are called from the main process (declared in face.h)

// Fork and start the face renderer as a child process
void start_face_thread(){
    sem_init(&face_ready_sem,0,0);
#ifndef _WIN32
    face_pid = fork();
    if(face_pid==0){
        // Child: close inherited file descriptors (except stdin/out/err)
        int mx = sysconf(_SC_OPEN_MAX);
        for(int fd=3; fd<mx; fd++) close(fd);
        run_face_process();  // never returns
    }
#endif
    sem_post(&face_ready_sem);  // signal ready to parent
}

// Kill the renderer child process
void stop_face_thread(){
#ifndef _WIN32
    if(face_pid>0){ kill(face_pid,SIGTERM); waitpid(face_pid,NULL,0); face_pid=-1; }
#endif
}

// Animate mouth open/close for a given text string
// Cycles open/closed once per word at ~100ms per state
void animate_mouth_for(const char *text){
    int w=0;
    for(int i=0; text[i]; i++) if(text[i]==' ') w++;
    w++;  // word count = spaces + 1
    for(int i=0; i<w*2; i++){
        mouth_open=(i%2);  // alternate open/closed
        FILE *f=fopen("/tmp/cloneme_mouth","w");
        if(f){ fprintf(f,"%d",mouth_open); fclose(f); }
        usleep(100000);  // 100ms per state
    }
    // Always end with mouth closed
    mouth_open=0;
    FILE *f=fopen("/tmp/cloneme_mouth","w");
    if(f){ fprintf(f,"0"); fclose(f); }
}

// Force mouth open (e.g. while TTS audio is playing)
void start_mouth_animation(void){
    mouth_open=1;
    FILE *f=fopen("/tmp/cloneme_mouth","w");
    if(f){ fprintf(f,"1"); fclose(f); }
}

// Force mouth closed
void stop_mouth_animation(void){
    mouth_open=0;
    FILE *f=fopen("/tmp/cloneme_mouth","w");
    if(f){ fprintf(f,"0"); fclose(f); }
}
