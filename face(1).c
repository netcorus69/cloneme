// face.c
// ─────────────────────────────────────────────────────────────────────────────
// CloneMe — 3D head renderer using SDL2 + OpenGL 3.3 Core
// Loads a .obj head mesh, applies a texture, and renders it in a window.
// Mouth animation is controlled via a shared file: /tmp/cloneme_mouth
// Modular morph system included (per-vertex mask + morph targets).
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
#include <time.h>
#define MOUTH_MORPH   0
#define EYELID_MORPH  1
#define PUPIL_MORPH   2

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

// Morph system
#define MAX_MORPHS 8
typedef struct { GLuint vbo; int vertex_count; char name[64]; } MorphTarget;
static MorphTarget morphs[MAX_MORPHS];
static int morph_count = 0;
static float morph_weights[MAX_MORPHS] = {0.0f};
static int is_talking = 0; // set to 1 when speaking, 0 when idle



// ─── SHADERS ──────────────────────────────────────────────────────────────────
// Shaders are written in GLSL 330 (OpenGL 3.3 Core)

// Vertex shader with morph support (locations: 0=pos,1=norm,2=uv,3=mask,4..=morphs)
static const char *vert_src =
"#version 330 core\n"
"layout(location=0) in vec3 aPos;\n"
"layout(location=1) in vec3 aNorm;\n"
"layout(location=2) in vec2 aUV;\n"
"layout(location=3) in float aMask;\n"
"layout(location=4) in vec3 aMorph0;\n"
"layout(location=5) in vec3 aMorph1;\n"
"layout(location=6) in vec3 aMorph2;\n"
"layout(location=7) in vec3 aMorph3;\n"
"layout(location=8) in vec3 aMorph4;\n"
"layout(location=9) in vec3 aMorph5;\n"
"layout(location=10) in vec3 aMorph6;\n"
"layout(location=11) in vec3 aMorph7;\n"
"out vec3 vN; out vec3 vP; out vec2 vUV; out float vMorphMag;\n"
"uniform mat4 MVP; uniform mat4 M;\n"
"uniform float uMorph[8];\n"
"uniform float uMorphScale;\n"
"void main(){\n"
"  vec3 morphs[8];\n"
"  morphs[0] = aMorph0; morphs[1] = aMorph1; morphs[2] = aMorph2; morphs[3] = aMorph3;\n"
"  morphs[4] = aMorph4; morphs[5] = aMorph5; morphs[6] = aMorph6; morphs[7] = aMorph7;\n"
"  vec3 delta = vec3(0.0);\n"
"  for(int i=0;i<8;i++){\n"
"    delta += uMorph[i] * (morphs[i] - aPos);\n"
"  }\n"
"  delta *= uMorphScale * aMask;\n"
"  vec3 pos = aPos + delta;\n"
"  vMorphMag = length(delta);\n"
"  vP = vec3(M * vec4(pos,1.0));\n"
"  vN = normalize(mat3(M) * aNorm);\n"
"  vUV = aUV;\n"
"  gl_Position = MVP * vec4(pos,1.0);\n"
"}\n";


// FRAGMENT SHADER — Normal visualization (debug)
static const char *frag_normal_src =
"#version 330 core\n"
"in vec3 vN; in vec3 vP; in vec2 vUV;\n"
"out vec4 C;\n"
"void main(){\n"
"  vec3 N = normalize(vN);\n" 
"  vec3 Nvis = N * 0.5 + 0.5;\n"
"  C = vec4(Nvis, 1.0);\n"
"}\n";

static const char *frag_final_src =
"#version 330 core\n"
"in vec3 vN; in vec3 vP; in vec2 vUV;\n"
"out vec4 C;\n"
"uniform sampler2D albedoTex;\n"
"void main(){\n"
"  vec3 N = normalize(vN);\n"
"  vec3 L = normalize(vec3(0.3, 1.0, 2.0));\n"
"  float diff = max(dot(N, L), 0.0);\n"
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

// ─── ROTATION MATRICES ───────────────────────────────────────────────────────
static void mrotx(M4 m, float a){
    mid(m); m[5]=cosf(a); m[9]=-sinf(a); m[6]=sinf(a); m[10]=cosf(a);
}
static void mroty(M4 m, float a){
    mid(m); m[0]=cosf(a); m[8]=sinf(a); m[2]=-sinf(a); m[10]=cosf(a);
}
static void mrotz(M4 m, float a){
    mid(m); m[0]=cosf(a); m[4]=sinf(a); m[1]=-sinf(a); m[5]=cosf(a);
}


// ─── SPHERE (fallback geometry) ───────────────────────────────────────────────
#define SL 32   // longitude slices
#define ST 20   // latitude stacks
static GLuint g_vao; static int g_ic;

static void init_sphere(){
    int vc=(SL+1)*(ST+1);
    float *v=malloc(vc*6*4); int p=0;
    for(int i=0;i<=ST;i++){
        float phi=(float)M_PI*i/ST-(float)M_PI/2;
        for(int j=0;j<=SL;j++){
            float th=2*(float)M_PI*j/SL;
            float nx=cosf(phi)*cosf(th), ny=sinf(phi), nz=cosf(phi)*sinf(th);
            v[p++]=nx; v[p++]=ny; v[p++]=nz;
            v[p++]=nx; v[p++]=ny; v[p++]=nz;
        }
    }
    g_ic=ST*SL*6; unsigned *idx=malloc(g_ic*4); p=0;
    for(int i=0;i<ST;i++) for(int j=0;j<SL;j++){
        int a=i*(SL+1)+j, b=a+SL+1;
        idx[p++]=a; idx[p++]=b; idx[p++]=a+1;
        idx[p++]=b; idx[p++]=b+1; idx[p++]=a+1;
    }
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



static float idle_angle = 0.0f;
static unsigned long last_idle_tick = 0;

void update_idle_head(M4 head_matrix, unsigned long tick) {
    if (!is_talking) {
        if (tick - last_idle_tick > 200) { // update every ~200ms
            last_idle_tick = tick;
            float delta = ((float)rand() / RAND_MAX - 0.5f) * 0.05f; // small random step
            idle_angle += delta;
            mrotz(head_matrix, idle_angle); // rotate head around Z
        }
    }
}


// ─── SHADER PROGRAM ───────────────────────────────────────────────────────────

static GLuint g_prog;   // active shader program ID
static M4 g_vp;         // combined View * Projection matrix (shared across draws)



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

// ─── OBJ PARSING HELPERS ──────────────────────────────────────────────────────
typedef struct { float x,y,z; } V3;
typedef struct { float u,v;   } UV;
typedef struct { float x,y,z; } N3;

static void parse3f(const char *ptr, float *a, float *b, float *c){
    char *e;
    *a = (float)strtod(ptr, &e);
    *b = (float)strtod(e,   &e);
    *c = (float)strtod(e,   NULL);
}

static void parse2f(const char *ptr, float *a, float *b){
    char *e;
    *a = (float)strtod(ptr, &e);
    *b = (float)strtod(e,   NULL);
}

// Load a simple mask file: one float per original position index (whitespace separated).
// Returns number of mask entries read, caller must free(*out_mask).
static int load_mask_file(const char *path, float **out_mask){
    FILE *f = fopen(path, "r");
    if(!f) return 0;
    int cap = 1024, n = 0;
    float *mask = malloc(sizeof(float) * cap);
    if(!mask){ fclose(f); return 0; }
    while(1){
        float v;
        if(fscanf(f, "%f", &v) != 1) break;
        if(n + 1 > cap){
            cap *= 2;
            float *tmp = realloc(mask, sizeof(float) * cap);
            if(!tmp) break;
            mask = tmp;
        }
        mask[n++] = v;
    }
    fclose(f);
    if(n == 0){ free(mask); return 0; }
    *out_mask = mask;
    return n;
}


// ─── MORPH LOADER (positions only) ───────────────────────────────────────────
// Loads only 'v' lines from an OBJ into a float array (x,y,z) in order.
// Caller must free(*out_positions). Returns 1 on success.
static int load_morph_obj_positions(const char *path, float **out_positions, int *out_count){
    FILE *f = fopen(path,"r");
    if(!f){ perror("load_morph_obj_positions fopen"); return 0; }
    float *pos = NULL; int n=0, cap=0;
    char line[512];
    while(fgets(line,sizeof(line),f)){
        if(line[0]=='v' && line[1]==' '){
            float x,y,z; parse3f(line+2,&x,&y,&z);
            if(n+1>cap){ cap = cap?cap*2:1024; pos = realloc(pos,sizeof(float)*cap*3); }
            pos[n*3+0]=x; pos[n*3+1]=y; pos[n*3+2]=z; n++;
        }
    }
    fclose(f);
    if(n==0){ free(pos); return 0; }
    *out_positions = pos; *out_count = n;
    return 1;
}

// Replacement loader: loads base OBJ and morphs, expanding morphs to match expanded base vertices.
// base_path: path to base OBJ (triangulated). morph_paths: array of morph OBJ filenames (raw v lists).
// Returns 1 on success, 0 on failure. Outputs VAO and index count.
static int load_obj_and_morphs_to_vao(const char *base_path, const char *morph_paths[], int morph_path_count,
                                      GLuint *out_vao, int *out_index_count)
{
    FILE *f = fopen(base_path,"r");
    if(!f){ perror("load_obj fopen"); fprintf(stderr,"OBJ open failed: %s\n",base_path); return 0; }

    V3 *pos=NULL; int pos_n=0, pos_cap=0;
    UV *uv =NULL; int uv_n=0,  uv_cap=0;
    N3 *nor=NULL; int nor_n=0, nor_cap=0;

    float    *vdata=NULL; int vdata_n=0, vdata_cap=0; // floats count
    unsigned *idx  =NULL; int idx_n=0,   idx_cap=0;

    // mapping: for each expanded vertex (vdata index), store original position index (0-based)
    int *base_pos_idx = NULL; 
    //int base_pos_idx_cap = 0;

    // DEBUG: temporary buffer to capture base expanded positions for comparison
    float *base_expanded_pos = NULL;
    int base_expanded_pos_cap = 0;
    int base_expanded_pos_count = 0;
    // allocate a reasonable initial capacity (will grow if needed)
    base_expanded_pos_cap = 500000; // adjust if you expect more vertices
    base_expanded_pos = malloc(sizeof(float) * base_expanded_pos_cap * 3);
    if(!base_expanded_pos){ fprintf(stderr,"Out of memory for base_expanded_pos\n"); base_expanded_pos_cap = 0; }
    

	// Try to load mask from working directory first, then from blender files folder as fallback
	float *masks = NULL;
	int mask_count = 0;
	mask_count = load_mask_file("monkey.mask", &masks);
	if(mask_count > 0){
		fprintf(stderr,"Loaded mask from working dir: monkey.mask (%d entries)\n", mask_count);
	} else {
		const char *fallback = "/home/netcorus/workfile/progetti/miei progetti/cloneme/blender files/monkey.mask";
		fprintf(stderr,"mask not found in CWD, trying fallback: %s\n", fallback);
		mask_count = load_mask_file(fallback, &masks);
		if(mask_count > 0){
			fprintf(stderr,"Loaded mask from fallback path: %s (%d entries)\n", fallback, mask_count);
		} else {
			fprintf(stderr,"No mask loaded (tried CWD and fallback). Continuing with zero masks.\n");
			if(masks){ free(masks); masks = NULL; }
			mask_count = 0;
		}
	}

    char line[1024];
    while(fgets(line,sizeof(line),f)){
        if(line[0]=='v' && line[1]==' '){
            float x,y,z; parse3f(line+2,&x,&y,&z);
            if(pos_n+1>pos_cap){ pos_cap = pos_cap?pos_cap*2:1024; pos = realloc(pos,sizeof(V3)*pos_cap); }
            pos[pos_n++] = (V3){x,y,z};
        } else if(line[0]=='v' && line[1]=='t'){
            float u0,v0; parse2f(line+3,&u0,&v0);
            if(uv_n+1>uv_cap){ uv_cap = uv_cap?uv_cap*2:1024; uv = realloc(uv,sizeof(UV)*uv_cap); }
            uv[uv_n++] = (UV){u0, 1.0f - v0}; // flip V
        } else if(line[0]=='v' && line[1]=='n'){
            float x,y,z; parse3f(line+3,&x,&y,&z);
            if(nor_n+1>nor_cap){ nor_cap = nor_cap?nor_cap*2:1024; nor = realloc(nor,sizeof(N3)*nor_cap); }
            nor[nor_n++] = (N3){x,y,z};
        } else if(line[0]=='f'){
            int vi[3]={0,0,0}, vti[3]={0,0,0}, vni[3]={0,0,0};
            int matches = sscanf(line+1,"%d/%d/%d %d/%d/%d %d/%d/%d",
                &vi[0],&vti[0],&vni[0], &vi[1],&vti[1],&vni[1], &vi[2],&vti[2],&vni[2]);
            if(matches<9){
                matches = sscanf(line+1,"%d//%d %d//%d %d//%d",
                    &vi[0],&vni[0], &vi[1],&vni[1], &vi[2],&vni[2]);
                if(matches<6){
                    sscanf(line+1,"%d/%d %d/%d %d/%d",
                        &vi[0],&vti[0], &vi[1],&vti[1], &vi[2],&vti[2]);
                }
            }
            for(int k=0;k<3;k++){
                int p = vi[k]-1, t = vti[k]-1, nidx = vni[k]-1;
                float px=0,py=0,pz=0, nx=0,ny=0,nz=0, u=0,vv=0;
                if(p>=0 && p<pos_n){ px=pos[p].x; py=pos[p].y; pz=pos[p].z; }
                if(t>=0 && t<uv_n ){ u=uv[t].u; vv=uv[t].v; }
                if(nidx>=0 && nidx<nor_n){ nx=nor[nidx].x; ny=nor[nidx].y; nz=nor[nidx].z; }

                // ensure capacity for vdata (9 floats per expanded vertex)
                if(vdata_n+9 > vdata_cap){
                    int old_vert_cap = vdata_cap ? (vdata_cap/9) : 0;
                    vdata_cap = vdata_cap? vdata_cap*2 : 16384;
                    vdata = realloc(vdata, sizeof(float)*vdata_cap);
                    // grow base_pos_idx to match new vertex capacity
                    int new_vert_cap = vdata_cap / 9;
                    base_pos_idx = realloc(base_pos_idx, sizeof(int) * new_vert_cap);
                    //base_pos_idx_cap = new_vert_cap;
                    // initialize new slots (optional)
                    for(int ii=old_vert_cap; ii<new_vert_cap; ++ii) base_pos_idx[ii] = -1;
                }

                int expanded_index = vdata_n / 9;
                base_pos_idx[expanded_index] = p;

                // store base expanded position for debug (grow buffer if needed)
                if(base_expanded_pos_cap > 0){
                    if(expanded_index >= base_expanded_pos_cap){
                        int newcap = base_expanded_pos_cap * 2;
                        float *tmp = realloc(base_expanded_pos, sizeof(float) * newcap * 3);
                        if(tmp){ base_expanded_pos = tmp; base_expanded_pos_cap = newcap; }
                    }
                    if(expanded_index < base_expanded_pos_cap){
                        base_expanded_pos[expanded_index*3 + 0] = px;
                        base_expanded_pos[expanded_index*3 + 1] = py;
                        base_expanded_pos[expanded_index*3 + 2] = pz;
                        if(expanded_index >= base_expanded_pos_count) base_expanded_pos_count = expanded_index + 1;
                    }
                }

                // choose mask from loaded mask file if available
				float mask = 0.0f;
				if(masks && p >= 0 && p < mask_count) mask = masks[p];

				// write into vdata (pos, norm, uv, mask)
				vdata[vdata_n++] = px; vdata[vdata_n++] = py; vdata[vdata_n++] = pz;
				vdata[vdata_n++] = nx; vdata[vdata_n++] = ny; vdata[vdata_n++] = nz;
				vdata[vdata_n++] = u;  vdata[vdata_n++] = vv;
				vdata[vdata_n++] = mask;


                if(idx_n+1 > idx_cap){
                    idx_cap = idx_cap? idx_cap*2 : 16384;
                    idx = realloc(idx, sizeof(unsigned)*idx_cap);
                }
                idx[idx_n++] = (unsigned)expanded_index;
            }
        }
    }
    fclose(f);

    if(vdata_n==0 || idx_n==0){ fprintf(stderr,"OBJ had no vertices/indices: %s\n", base_path);
        free(pos); free(uv); free(nor); free(vdata); free(idx); free(base_pos_idx);
        if(base_expanded_pos) free(base_expanded_pos);
        return 0; }

    int vert_count = vdata_n / 9;
    fprintf(stderr,"Loaded base: %d vertices, %d indices from %s\n", vert_count, idx_n, base_path);

    // Create VAO/VBO/EBO for base mesh
    GLuint vao, vbo, ebo;
    glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&vbo); glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float)*vdata_n, vdata, GL_STATIC_DRAW);
    glGenBuffers(1,&ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned)*idx_n, idx, GL_STATIC_DRAW);

    GLsizei stride = 9 * sizeof(float);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,stride,(void*)(0));               // pos
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,stride,(void*)(3*sizeof(float))); // norm
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,stride,(void*)(6*sizeof(float))); // uv
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3,1,GL_FLOAT,GL_FALSE,stride,(void*)(8*sizeof(float))); // mask
    glEnableVertexAttribArray(3);

    // free CPU-side base arrays (GPU has data)
    free(pos); free(uv); free(nor); free(vdata); free(idx);

    // Load morphs and expand them to match vert_count using base_pos_idx
    morph_count = 0;
    for(int m=0; m < morph_path_count && morph_count < MAX_MORPHS; ++m){
        const char *mpath = morph_paths[m];
        float *mpos = NULL; int mcount = 0;
        if(!load_morph_obj_positions(mpath, &mpos, &mcount)){
            fprintf(stderr,"Morph file not loaded (skipping): %s\n", mpath);
            continue;
        }
        // mcount is raw vertex count in morph file
        if(mcount <= 0){
            free(mpos);
            continue;
        }

        // Expand morph positions to match expanded base vertices
        float *morph_expanded = malloc(sizeof(float) * vert_count * 3);
        if(!morph_expanded){
            fprintf(stderr,"Out of memory expanding morph %s\n", mpath);
            free(mpos);
            continue;
        }
        for(int i=0;i<vert_count;i++){
            int orig_idx = base_pos_idx[i]; // original pos index (0-based)
            if(orig_idx < 0 || orig_idx >= mcount){
                // fallback: use base position (no delta) by copying base pos into morph_expanded
                // but we don't have base pos here; set to zero to avoid garbage
                morph_expanded[i*3 + 0] = 0.0f;
                morph_expanded[i*3 + 1] = 0.0f;
                morph_expanded[i*3 + 2] = 0.0f;
            } else {
                morph_expanded[i*3 + 0] = mpos[orig_idx*3 + 0];
                morph_expanded[i*3 + 1] = mpos[orig_idx*3 + 1];
                morph_expanded[i*3 + 2] = mpos[orig_idx*3 + 2];
            }
        }

        

        // upload expanded morph to GPU and bind to attribute location (4 + morph_count)
        GLuint morph_vbo; glGenBuffers(1,&morph_vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, morph_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float)*vert_count*3, morph_expanded, GL_STATIC_DRAW);
        int attr_loc = 4 + morph_count;
        glEnableVertexAttribArray(attr_loc);
        glVertexAttribPointer(attr_loc, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);

        // store morph metadata
        morphs[morph_count].vbo = morph_vbo;
        morphs[morph_count].vertex_count = vert_count;
        strncpy(morphs[morph_count].name, mpath, 63);
        morphs[morph_count].name[63] = '\0';
        morph_count++;

        free(mpos);
        free(morph_expanded);
        fprintf(stderr,"Loaded morph %d: %s (expanded verts=%d) bound to attr %d\n", morph_count-1, mpath, vert_count, attr_loc);
    }

    // cleanup mapping
    free(base_pos_idx);
    if(masks){ free(masks); masks = NULL; mask_count = 0; }


    // free debug base_expanded_pos buffer
    if(base_expanded_pos){
        free(base_expanded_pos);
        base_expanded_pos = NULL;
        base_expanded_pos_cap = 0;
        base_expanded_pos_count = 0;
    }

    *out_vao = vao;
    *out_index_count = idx_n;
    return 1;
}


// ─── MORPH MANAGER ───────────────────────────────────────────────────────────
static void set_morph_weight(int id, float w){
    if(id<0 || id>=MAX_MORPHS) return;
    morph_weights[id] = fmaxf(0.0f, fminf(1.0f, w));
}

static void upload_morph_uniforms(GLuint prog){
    glUseProgram(prog);
    // set morph array elements
    for(int i=0;i<MAX_MORPHS;i++){
        char name[32];
        snprintf(name, sizeof(name), "uMorph[%d]", i);
        GLint loc = glGetUniformLocation(prog, name);
        if(loc >= 0){
            glUniform1f(loc, morph_weights[i]);
        }
    }
    // set morph scale (amplify small morphs for debug; set to 1.0 for normal)
    GLint locScale = glGetUniformLocation(prog, "uMorphScale");
    if(locScale >= 0) glUniform1f(locScale, 1.0f);
    // optional debug amplify uniform for fragment shader
    GLint locDbg = glGetUniformLocation(prog, "debugAmplify");
    if(locDbg >= 0) glUniform1f(locDbg, 10.0f);
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
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    stbi_image_free(data);
    printf("[DEBUG] Loaded texture %s (%dx%d, channels=%d), ID=%u\n",path,w,h,n,tex);
    return tex;
}

void update_idle_eyes(M4 eye_matrix_left, M4 eye_matrix_right, int is_talking) {
    float t = SDL_GetTicks() * 0.001f;

    // Gentle drift
    float offset_x = 0.02f * sinf(t * 1.3f);
    float offset_y = 0.01f * cosf(t * 0.9f);

    if (!is_talking) {
        mtrans(eye_matrix_left,  offset_x, offset_y, 0.0f);
        mtrans(eye_matrix_right, offset_x, offset_y, 0.0f);
    } else {
        mtrans(eye_matrix_left, 0.0f, 0.0f, 0.0f);
        mtrans(eye_matrix_right,0.0f, 0.0f, 0.0f);
    }
}

void update_blink() {
    static unsigned long last_blink = 0;
    static int blinking = 0;
    unsigned long now = SDL_GetTicks();

    if (!blinking && now - last_blink > (3000u + rand()%4000u)) {
        blinking = 1;
        last_blink = now;
    }

    if (blinking) {
        float phase = (now - last_blink) / 200.0f; // 200ms blink
        if (phase < 1.0f) {
            set_morph_weight(EYELID_MORPH, sinf(phase * M_PI)); // close then open
        } else {
            set_morph_weight(EYELID_MORPH, 0.0f);
            blinking = 0;
        }
    }
}




// ─── RENDER ───────────────────────────────────────────────────────────────────


static void render(int m, int tick __attribute__((unused))) {
    glUseProgram(g_prog);
    


    // Mouth morph
    static float s_mouth = 0.0f;
    float target = m ? 1.0f : 0.0f;
    s_mouth += (target - s_mouth) * 0.2f;
    set_morph_weight(MOUTH_MORPH, s_mouth);

    // Eyelid blink
    static unsigned long last_blink = 0;
    static int blinking = 0;
    unsigned long now = SDL_GetTicks();
    if (!blinking && now - last_blink > (3000u + rand()%4000u)) {
        blinking = 1;
        last_blink = now;
    }
    if (blinking) {
        float phase = (now - last_blink) / 200.0f;
        if (phase < 1.0f) {
            set_morph_weight(EYELID_MORPH, sinf(phase * M_PI));
        } else {
            set_morph_weight(EYELID_MORPH, 0.0f);
            blinking = 0;
        }
    }

    // Upload morphs
    upload_morph_uniforms(g_prog);
    glUniform1f(glGetUniformLocation(g_prog,"uMorph0"), morph_weights[MOUTH_MORPH]);
    glUniform1f(glGetUniformLocation(g_prog,"uMorph1"), morph_weights[EYELID_MORPH]);

    // Head idle animation
    if(g_head_vao && g_head_idx_count){
        M4 S, Rx, Ry, Rz, T, tmp, tmp2, tmp3, M_mat, MVP;
        mscl(S,   0.12f, 0.12f, 0.12f);

        float t = SDL_GetTicks() * 0.001f;
        float angle_y = 0.05f * sinf(t);        // sway left/right
        float angle_x = 0.03f * cosf(t*0.7f);   // gentle nod
        float angle_z = 0.02f * sinf(t*0.5f);   // slight roll

        static float damp_x = 0.0f, damp_y = 0.0f, damp_z = 0.0f;
        if (!is_talking) {
            damp_x += (angle_x - damp_x) * 0.05f;
            damp_y += (angle_y - damp_y) * 0.05f;
            damp_z += (angle_z - damp_z) * 0.05f;
        } else {
            damp_x *= 0.9f;
            damp_y *= 0.9f;
            damp_z *= 0.9f;
        }

        mrotx(Rx, damp_x);
        mroty(Ry, damp_y);
        mrotz(Rz, damp_z);

        mtrans(T, 0.0f, 0.0f, -1.0f);
        mmul(tmp,   Ry,   Rx);
        mmul(tmp3,  tmp,  Rz);
        mmul(tmp2,  S,    tmp3);
        mmul(M_mat, T,    tmp2);
        mmul(MVP,   g_vp, M_mat);

        glUniformMatrix4fv(glGetUniformLocation(g_prog,"MVP"),1,0,MVP);
        glUniformMatrix4fv(glGetUniformLocation(g_prog,"M"),1,0,M_mat);
        glBindVertexArray(g_head_vao);
        glDrawElements(GL_TRIANGLES,g_head_idx_count,GL_UNSIGNED_INT,0);
    }
}



// ─── FACE PROCESS ─────────────────────────────────────────────────────────────
static void run_face_process(){
    printf("[DEBUG] Initializing SDL...\n");

    SDL_Init(SDL_INIT_VIDEO);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS,1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES,4);

    SDL_Window *win = SDL_CreateWindow("CloneMe",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        400, 450, SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN);

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    glewExperimental = GL_TRUE; glewInit();

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);
    printf("[DEBUG] SDL window and GL context created.\n");

    setlocale(LC_NUMERIC,"C");
    printf("[DEBUG] Locale set to C (dot decimal)\n");

    frag_src = g_normal_vis ? frag_normal_src : frag_final_src;
    g_prog = mkprog();
    if(!g_prog) fprintf(stderr,"[DEBUG] Warning: shader program creation failed\n");

    init_sphere();
    glUseProgram(g_prog);

    // Load head mesh + morphs
    const char *morph_files[] = {"monkey_open.obj", "monkey_wide.obj"}; // edit as needed
    if(!load_obj_and_morphs_to_vao("monkey.obj", morph_files, 2, &g_head_vao, &g_head_idx_count)){
        fprintf(stderr,"[DEBUG] Failed to load head.obj — using sphere fallback\n");
    } else {
        printf("[DEBUG] Head mesh loaded: VAO=%u, indices=%d\n",g_head_vao,g_head_idx_count);
    }

    // Load texture
    g_albedo_tex = load_texture("monkey.png");
    glUseProgram(g_prog);
    glUniform1i(glGetUniformLocation(g_prog,"albedoTex"),0);
    printf("[DEBUG] Albedo texture ID=%u\n",g_albedo_tex);

    if(g_albedo_tex==0){
        unsigned char white[3]={255,255,255};
        glGenTextures(1,&g_albedo_tex);
        glBindTexture(GL_TEXTURE_2D,g_albedo_tex);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,1,1,0,GL_RGB,GL_UNSIGNED_BYTE,white);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        fprintf(stderr,"[DEBUG] Created 1x1 white fallback texture\n");
    }

    M4 view,proj;
    mtrans(view,0,.10f,-4.5f);
    mpersp(proj,.52f,400.f/450.f,.1f,50.f);
    mmul(g_vp,proj,view);

    int m=0, tick=0;
    SDL_Event e;
    while(1){
        while(SDL_PollEvent(&e)) if(e.type==SDL_QUIT) goto done;

        FILE *f = fopen("/tmp/cloneme_mouth","r");
        if(f){ if(fscanf(f,"%d",&m)!=1) m=0; fclose(f); }

        glClearColor(.22f,.22f,.28f,1.f);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        render(m,tick);
        SDL_GL_SwapWindow(win);
        SDL_Delay(33);
        tick++;
    }
done:
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    exit(0);
}

// ─── PUBLIC API ───────────────────────────────────────────────────────────────
void start_face_thread(){
    sem_init(&face_ready_sem,0,0);
#ifndef _WIN32
    face_pid = fork();
    if(face_pid==0){
        int mx = sysconf(_SC_OPEN_MAX);
        for(int fd=3; fd<mx; fd++) close(fd);
        run_face_process();
    }
#endif
    sem_post(&face_ready_sem);
}

void stop_face_thread(){
#ifndef _WIN32
    if(face_pid>0){ kill(face_pid,SIGTERM); waitpid(face_pid,NULL,0); face_pid=-1; }
#endif
}

void animate_mouth_for(const char *text){
    int w=0;
    for(int i=0; text[i]; i++) if(text[i]==' ') w++;
    w++;
    for(int i=0; i<w*2; i++){
        mouth_open=(i%2);
        FILE *f=fopen("/tmp/cloneme_mouth","w");
        if(f){ fprintf(f,"%d",mouth_open); fclose(f); }
        usleep(100000);
    }
    mouth_open=0;
    FILE *f=fopen("/tmp/cloneme_mouth","w");
    if(f){ fprintf(f,"0"); fclose(f); }
}

void start_mouth_animation(void){
    mouth_open=1;
    FILE *f=fopen("/tmp/cloneme_mouth","w");
    if(f){ fprintf(f,"1"); fclose(f); }
}

void stop_mouth_animation(void){
    mouth_open=0;
    FILE *f=fopen("/tmp/cloneme_mouth","w");
    if(f){ fprintf(f,"0"); fclose(f); }
}
