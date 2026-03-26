// face.c
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

// stb_image (single-file)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ─── GLOBALS ──────────────────────────────────────────────────────────────────

static pid_t face_pid = -1;
int mouth_open = 0;
sem_t face_ready_sem;

// Mesh fallback globals
static GLuint g_head_vao = 0;
static int    g_head_idx_count = 0;
static GLuint g_albedo_tex = 0;

// Toggle shaders: 0 = final shader, 1 = normal visualization
static int g_normal_vis = 0;

// ─── SHADERS ──────────────────────────────────────────────────────────────────

static const char *vert_src =
"#version 330 core\n"
"layout(location=0) in vec3 aPos;\n"
"layout(location=1) in vec3 aNorm;\n"
"layout(location=2) in vec2 aUV;\n"
"out vec3 vN; out vec3 vP; out vec2 vUV;\n"
"uniform mat4 MVP; uniform mat4 M;\n"
"void main(){\n"
"  vP = vec3(M * vec4(aPos,1.0));\n"
"  vN = normalize(mat3(M) * aNorm);\n"
"  vUV = aUV;\n"
"  gl_Position = MVP * vec4(aPos,1.0);\n"
"}\n";

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
"in vec2 vUV;\n"
"out vec4 C;\n"
"uniform sampler2D albedoTex;\n"
"void main(){\n"
"  vec4 t = texture(albedoTex, vUV);\n"
"  C = vec4(t.rgb * 1.0, 1.0);\n"  // ← was 3.0
"}\n";

static const char *frag_src = NULL;

// ─── MATH ─────────────────────────────────────────────────────────────────────

typedef float M4[16];
static void mid(M4 m){memset(m,0,64);m[0]=m[5]=m[10]=m[15]=1;}
static void mmul(M4 r,const M4 a,const M4 b){
    M4 t;memset(t,0,64);
    for(int i=0;i<4;i++)for(int j=0;j<4;j++)for(int k=0;k<4;k++)
        t[j*4+i]+=a[k*4+i]*b[j*4+k];
    memcpy(r,t,64);
}
static void mpersp(M4 m,float fv,float asp,float n,float f){
    memset(m,0,64);float t=tanf(fv*.5f);
    m[0]=1/(asp*t);m[5]=1/t;m[10]=(f+n)/(n-f);m[14]=2*f*n/(n-f);m[11]=-1;
}
static void mtrans(M4 m,float x,float y,float z){mid(m);m[12]=x;m[13]=y;m[14]=z;}
static void mscl(M4 m,float x,float y,float z){mid(m);m[0]=x;m[5]=y;m[10]=z;}

// ─── SPHERE ───────────────────────────────────────────────────────────────────

#define SL 32
#define ST 20
static GLuint g_vao; static int g_ic;

static void init_sphere(){
    int vc=(SL+1)*(ST+1);
    float *v=malloc(vc*6*4); int p=0;
    for(int i=0;i<=ST;i++){
        float phi=(float)M_PI*i/ST-(float)M_PI/2;
        for(int j=0;j<=SL;j++){
            float th=2*(float)M_PI*j/SL;
            float nx=cosf(phi)*cosf(th),ny=sinf(phi),nz=cosf(phi)*sinf(th);
            v[p++]=nx;v[p++]=ny;v[p++]=nz;v[p++]=nx;v[p++]=ny;v[p++]=nz;
        }
    }
    g_ic=ST*SL*6; unsigned *idx=malloc(g_ic*4); p=0;
    for(int i=0;i<ST;i++)for(int j=0;j<SL;j++){
        int a=i*(SL+1)+j,b=a+SL+1;
        idx[p++]=a;idx[p++]=b;idx[p++]=a+1;
        idx[p++]=b;idx[p++]=b+1;idx[p++]=a+1;
    }
    GLuint vbo,ebo;
    glGenVertexArrays(1,&g_vao);glGenBuffers(1,&vbo);glGenBuffers(1,&ebo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,vc*24,v,GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,g_ic*4,idx,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,24,(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,24,(void*)12);glEnableVertexAttribArray(1);
    free(v);free(idx);
}

// ─── PROGRAM / DRAW HELPERS ───────────────────────────────────────────────────

static GLuint g_prog; static M4 g_vp;

static void dp(float tx,float ty,float tz,
               float sx,float sy,float sz,
               float r,float g,float b,float sp){
    M4 M,S,T,MVP;
    mtrans(T,tx,ty,tz);mscl(S,sx,sy,sz);mmul(M,T,S);mmul(MVP,g_vp,M);
    glUniformMatrix4fv(glGetUniformLocation(g_prog,"MVP"),1,0,MVP);
    glUniformMatrix4fv(glGetUniformLocation(g_prog,"M"),1,0,M);
    glUniform3f(glGetUniformLocation(g_prog,"col"),r,g,b);
    glUniform1f(glGetUniformLocation(g_prog,"sp"),sp);
    glBindVertexArray(g_vao);
    glDrawElements(GL_TRIANGLES,g_ic,GL_UNSIGNED_INT,0);
}

static GLuint compile_shader(GLenum type, const char *src){
    GLuint id = glCreateShader(type);
    glShaderSource(id, 1, &src, NULL);
    glCompileShader(id);
    GLint ok = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if(!ok){
        char log[4096]; glGetShaderInfoLog(id, sizeof(log), NULL, log);
        fprintf(stderr, "Shader compile error: %s\n", log);
        glDeleteShader(id);
        return 0;
    }
    return id;
}

static GLuint mkprog(void){
    if(!frag_src) frag_src = g_normal_vis ? frag_normal_src : frag_final_src;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if(!vs || !fs){
        if(vs) glDeleteShader(vs);
        if(fs) glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if(!ok){
        char log[4096]; glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "[DEBUG] Program link error: %s\n", log);
        glDeleteProgram(p);
        p = 0;
    } else {
        printf("[DEBUG] Shader program linked successfully, ID=%u\n", p);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

// ─── OBJ LOADER ───────────────────────────────────────────────────────────────

typedef struct { float x,y,z; } V3;
typedef struct { float u,v; } UV;
typedef struct { float x,y,z; } N3;

// ✅ FIX: locale-safe helper — parses 3 floats using strtod (always dot-decimal)
static void parse3f(const char *ptr, float *a, float *b, float *c){
    char *e;
    *a = (float)strtod(ptr, &e);
    *b = (float)strtod(e,   &e);
    *c = (float)strtod(e,   NULL);
}

// ✅ FIX: locale-safe helper — parses 2 floats using strtod
static void parse2f(const char *ptr, float *a, float *b){
    char *e;
    *a = (float)strtod(ptr, &e);
    *b = (float)strtod(e,   NULL);
}

static int load_obj_to_vao(const char *path, GLuint *out_vao, int *out_index_count){
    FILE *f = fopen(path,"r");
    if(!f){ perror("load_obj_to_vao fopen"); fprintf(stderr,"OBJ open failed: %s\n", path); return 0; }

    V3 *pos = NULL; int pos_n=0, pos_cap=0;
    UV *uv  = NULL; int uv_n=0, uv_cap=0;
    N3 *nor = NULL; int nor_n=0, nor_cap=0;

    float *vdata = NULL; int vdata_n=0, vdata_cap=0;
    unsigned *idx = NULL; int idx_n=0, idx_cap=0;

    char line[512];
    while(fgets(line,sizeof(line),f)){

        // ✅ FIX: was sscanf(..., "%f %f %f", ...) — now uses parse3f (strtod)
        if(line[0]=='v' && line[1]==' '){
            float x,y,z;
            parse3f(line+2, &x, &y, &z);
            if(pos_n+1>pos_cap){ pos_cap = pos_cap?pos_cap*2:256; pos = realloc(pos,sizeof(V3)*pos_cap); }
            pos[pos_n++] = (V3){x,y,z};
        }

        // ✅ FIX: was mixed strtod but inconsistent — now clean parse2f
        else if(line[0]=='v' && line[1]=='t'){
            float u, v;
            parse2f(line+3, &u, &v);
            printf("[DEBUG] vt u=%f v=%f\n", u, v);
            if(uv_n+1 > uv_cap){
                uv_cap = uv_cap ? uv_cap*2 : 256;
                uv = realloc(uv, sizeof(UV)*uv_cap);
            }
            uv[uv_n++] = (UV){u, 1.0f - v};
        }

        // ✅ FIX: was sscanf(..., "%f %f %f", ...) — now uses parse3f (strtod)
        else if(line[0]=='v' && line[1]=='n'){
            float x,y,z;
            parse3f(line+3, &x, &y, &z);
            if(nor_n+1>nor_cap){ nor_cap = nor_cap?nor_cap*2:256; nor = realloc(nor,sizeof(N3)*nor_cap); }
            nor[nor_n++] = (N3){x,y,z};
        }

        else if(line[0]=='f'){
            int vi[3], vti[3], vni[3];
            for(int i=0;i<3;i++){ vi[i]=vti[i]=vni[i]=0; }
            int matches = sscanf(line+1, "%d/%d/%d %d/%d/%d %d/%d/%d",
                                 &vi[0],&vti[0],&vni[0], &vi[1],&vti[1],&vni[1], &vi[2],&vti[2],&vni[2]);
            if(matches < 9){
                matches = sscanf(line+1, "%d//%d %d//%d %d//%d",
                                 &vi[0],&vni[0], &vi[1],&vni[1], &vi[2],&vni[2]);
                if(matches<6){
                    sscanf(line+1, "%d/%d %d/%d %d/%d",
                           &vi[0],&vti[0], &vi[1],&vti[1], &vi[2],&vti[2]);
                }
            }
            for(int k=0;k<3;k++){
                int p = vi[k]-1;
                int t = vti[k]-1;
                int n = vni[k]-1;
                float px=0,py=0,pz=0, nx=0,ny=0,nz=0, u=0,vv=0;
                if(p>=0 && p<pos_n){ px=pos[p].x; py=pos[p].y; pz=pos[p].z; }
                if(t>=0 && t<uv_n){ u=uv[t].u; vv=uv[t].v; }
                if(n>=0 && n<nor_n){ nx=nor[n].x; ny=nor[n].y; nz=nor[n].z; }

                int cur = vdata_n / 8;

                if(vdata_n + 8 > vdata_cap){
                    vdata_cap = vdata_cap ? vdata_cap * 2 : 4096;
                    vdata = realloc(vdata, sizeof(float) * vdata_cap);
                }
                vdata[vdata_n++]=px; vdata[vdata_n++]=py; vdata[vdata_n++]=pz;
                vdata[vdata_n++]=nx; vdata[vdata_n++]=ny; vdata[vdata_n++]=nz;
                vdata[vdata_n++]=u;  vdata[vdata_n++]=vv;

                if(idx_n + 1 > idx_cap){
                    idx_cap = idx_cap ? idx_cap * 2 : 4096;
                    idx = realloc(idx, sizeof(unsigned) * idx_cap);
                }
                idx[idx_n++] = (unsigned)cur;
            }
        }
    }
    fclose(f);
    if(vdata_n==0){ fprintf(stderr,"OBJ had no vertices: %s\n", path); return 0; }

    GLuint vao,vbo,ebo;
    glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&vbo); glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float)*vdata_n, vdata, GL_STATIC_DRAW);
    glGenBuffers(1,&ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned)*idx_n, idx, GL_STATIC_DRAW);

    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(6*sizeof(float)));
    glEnableVertexAttribArray(2);
    fprintf(stderr,"Loaded %d vertices, %d indices from %s\n", vdata_n/8, idx_n, path);

    free(pos); free(uv); free(nor); free(vdata); free(idx);

    *out_vao = vao;
    *out_index_count = idx_n;
    return 1;
}

// ─── DRAW MESH ────────────────────────────────────────────────────────────────

static void draw_mesh_with_dp(GLuint vao, int index_count,
                              float tx,float ty,float tz,
                              float sx,float sy,float sz,
                              float r,float g,float b,float sp)
{
    if(vao==0 || index_count==0) return;

    M4 M,S,T,MVP;
    mtrans(T,tx,ty,tz);
    mscl(S,sx,sy,sz);
    mmul(M,T,S);
    mmul(MVP,g_vp,M);

    glUniformMatrix4fv(glGetUniformLocation(g_prog,"MVP"),1,0,MVP);
    glUniformMatrix4fv(glGetUniformLocation(g_prog,"M"),1,0,M);
    glUniform3f(glGetUniformLocation(g_prog,"col"), r,g,b);
    glUniform1f(glGetUniformLocation(g_prog,"sp"), sp);

    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_INT, 0);
}

// ─── TEXTURE LOADER ───────────────────────────────────────────────────────────

static GLuint load_texture(const char *path){
    int w,h,n;
    unsigned char *data = stbi_load(path, &w, &h, &n, 3);
    if(!data){
        fprintf(stderr, "[DEBUG] Texture load failed for %s: %s\n", path, stbi_failure_reason());
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
    printf("[DEBUG] Loaded texture %s (%dx%d, channels=%d), ID=%u\n", path, w, h, n, tex);
    return tex;
}

// ─── RENDER ───────────────────────────────────────────────────────────────────

static void mrotx(M4 m, float a){
    mid(m); m[5]=cosf(a); m[9]=-sinf(a); m[6]=sinf(a); m[10]=cosf(a);
}

static void mroty(M4 m, float a){
    mid(m); m[0]=cosf(a); m[8]=sinf(a); m[2]=-sinf(a); m[10]=cosf(a);
}

static void mrotz(M4 m, float a){
    mid(m); m[0]=cosf(a); m[4]=-sinf(a); m[1]=sinf(a); m[5]=cosf(a);
}

static void render(int m, int tick){
    glUseProgram(g_prog);
    glUniform3f(glGetUniformLocation(g_prog,"col"), 1.0f, 1.0f, 1.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_albedo_tex);

    if(g_head_vao && g_head_idx_count){
        M4 S, Rx, Ry, T, tmp, tmp2, M_mat, MVP;

mscl(S,   0.12f, 0.12f, 0.12f);
mrotx(Rx, -1.5708f);
mroty(Ry,  3.1416f);
mtrans(T, 0.0f, -1.1f, -3.5f);  // ← was 0.5f, now -1.1f it move down 
mmul(tmp,   Ry,   Rx);    // ← Ry first, then Rx
mmul(tmp2,  S,    tmp);
mmul(M_mat, T,    tmp2);
mmul(MVP,   g_vp, M_mat);

        glUniformMatrix4fv(glGetUniformLocation(g_prog,"MVP"),1,0,MVP);
        glUniformMatrix4fv(glGetUniformLocation(g_prog,"M"),1,0,M_mat);
        glBindVertexArray(g_head_vao);
        glDrawElements(GL_TRIANGLES, g_head_idx_count, GL_UNSIGNED_INT, 0);
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

    SDL_Window *win=SDL_CreateWindow("CloneMe",
        SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
        400,450,SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN);
    SDL_GLContext ctx=SDL_GL_CreateContext(win);
    glewExperimental=GL_TRUE; glewInit();
    glEnable(GL_DEPTH_TEST); glEnable(GL_MULTISAMPLE);
    printf("[DEBUG] SDL window and GL context created.\n");

    // ✅ FIX: SDL resets locale — force dot-decimal AFTER all SDL/GL init
    setlocale(LC_NUMERIC, "C");
    printf("[DEBUG] Locale set to C (dot decimal)\n");

    frag_src = g_normal_vis ? frag_normal_src : frag_final_src;
    g_prog = mkprog();
    if(!g_prog) fprintf(stderr,"[DEBUG] Warning: shader program creation failed\n");

    init_sphere(); glUseProgram(g_prog);

    if(!load_obj_to_vao("head_fixed_uv.obj",&g_head_vao,&g_head_idx_count)){
        fprintf(stderr,"[DEBUG] Failed to load head_uv.obj — using sphere fallback\n");
    } else {
        printf("[DEBUG] Head mesh loaded: VAO=%u, indices=%d\n", g_head_vao, g_head_idx_count);
    }

    g_albedo_tex = load_texture("face_skinbg.png");
    glUseProgram(g_prog);
    glUniform1i(glGetUniformLocation(g_prog, "albedoTex"), 0);
    printf("[DEBUG] Albedo texture ID=%u\n", g_albedo_tex);

    if(g_albedo_tex == 0){
        unsigned char white[3] = {255,255,255};
        glGenTextures(1, &g_albedo_tex);
        glBindTexture(GL_TEXTURE_2D, g_albedo_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        fprintf(stderr,"[DEBUG] Created 1x1 white fallback texture for albedo.\n");
    }

    M4 view,proj;
    mtrans(view,0,.10f,-4.5f);
    mpersp(proj,.52f,400.f/450.f,.1f,50.f);
    mmul(g_vp,proj,view);

    int m=0,tick=0; SDL_Event e;
    while(1){
        while(SDL_PollEvent(&e)) if(e.type==SDL_QUIT) goto done;
        FILE *f=fopen("/tmp/cloneme_mouth","r");
        if(f){if(fscanf(f,"%d",&m)!=1)m=0;fclose(f);}
        glClearColor(.22f,.22f,.28f,1.f);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        render(m,tick);
        SDL_GL_SwapWindow(win);
        SDL_Delay(33); tick++;
    }
done:
    SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit(); exit(0);
}

// ─── PUBLIC API ───────────────────────────────────────────────────────────────

void start_face_thread(){
    sem_init(&face_ready_sem,0,0);
#ifndef _WIN32
    face_pid=fork();
    if(face_pid==0){
        int mx=sysconf(_SC_OPEN_MAX);
        for(int fd=3;fd<mx;fd++) close(fd);
        run_face_process();
    }
#endif
    sem_post(&face_ready_sem);
}

void stop_face_thread(){
#ifndef _WIN32
    if(face_pid>0){kill(face_pid,SIGTERM);waitpid(face_pid,NULL,0);face_pid=-1;}
#endif
}

void animate_mouth_for(const char *text){
    int w=0; for(int i=0;text[i];i++) if(text[i]==' ')w++;
    w++;
    for(int i=0;i<w*2;i++){
        mouth_open=(i%2);
        FILE *f=fopen("/tmp/cloneme_mouth","w");
        if(f){fprintf(f,"%d",mouth_open);fclose(f);}
        usleep(100000);
    }
    mouth_open=0;
    FILE *f=fopen("/tmp/cloneme_mouth","w");
    if(f){fprintf(f,"0");fclose(f);}
}

void start_mouth_animation(void){
    mouth_open=1;
    FILE *f=fopen("/tmp/cloneme_mouth","w");
    if(f){fprintf(f,"1");fclose(f);}
}

void stop_mouth_animation(void){
    mouth_open=0;
    FILE *f=fopen("/tmp/cloneme_mouth","w");
    if(f){fprintf(f,"0");fclose(f);}
}
