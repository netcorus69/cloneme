// face2d.c  — Live2D-style 2D animated face
// Drop-in replacement for face.c
// Keeps same public API: start_face_thread, stop_face_thread, animate_mouth_for

#include "face.h"
#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <locale.h>
#include <semaphore.h>
#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ─── GLOBALS ──────────────────────────────────────────────────────────────────

static pid_t face_pid = -1;
int mouth_open = 0;
sem_t face_ready_sem;

// ─── GRID CONFIG ──────────────────────────────────────────────────────────────

#define COLS 24        // horizontal subdivisions
#define ROWS 28        // vertical subdivisions
#define VERT_COUNT ((COLS+1)*(ROWS+1))
#define IDX_COUNT  (COLS*ROWS*6)

// Base positions (NDC -1..1) and UVs
static float base_x[VERT_COUNT];
static float base_y[VERT_COUNT];
static float uv_s[VERT_COUNT];
static float uv_t[VERT_COUNT];

// Animated positions (updated every frame)
static float cur_x[VERT_COUNT];
static float cur_y[VERT_COUNT];

static GLuint g_vao, g_vbo, g_ebo;
static GLuint g_prog;
static GLuint g_tex;
static unsigned g_idx[IDX_COUNT];

// ─── FACE REGIONS (normalized 0..1 in UV space) ──────────────────────────────

// Mouth region
#define MOUTH_U0  0.30f
#define MOUTH_U1  0.70f
#define MOUTH_V0  0.66f
#define MOUTH_V1  0.80f

// Eye left region
#define LEYE_U0   0.24f
#define LEYE_U1   0.46f
#define LEYE_V0   0.42f
#define LEYE_V1   0.52f

// Eye right region
#define REYE_U0   0.54f
#define REYE_U1   0.76f
#define REYE_V0   0.42f
#define REYE_V1   0.52f

// ─── SHADERS ──────────────────────────────────────────────────────────────────

static const char *vert_src =
"#version 330 core\n"
"layout(location=0) in vec2 aPos;\n"
"layout(location=1) in vec2 aUV;\n"
"out vec2 vUV;\n"
"void main(){\n"
"  vUV = aUV;\n"
"  gl_Position = vec4(aPos, 0.0, 1.0);\n"
"}\n";

static const char *frag_src =
"#version 330 core\n"
"in vec2 vUV;\n"
"out vec4 C;\n"
"uniform sampler2D tex;\n"
"void main(){\n"
"  C = texture(tex, vUV);\n"
"}\n";

// ─── SHADER COMPILE ───────────────────────────────────────────────────────────

static GLuint compile(GLenum type, const char *src){
    GLuint id = glCreateShader(type);
    glShaderSource(id,1,&src,NULL);
    glCompileShader(id);
    GLint ok=0; glGetShaderiv(id,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[2048]; glGetShaderInfoLog(id,sizeof(log),NULL,log);
             fprintf(stderr,"Shader error: %s\n",log); return 0; }
    return id;
}

static GLuint mkprog(){
    GLuint vs=compile(GL_VERTEX_SHADER,vert_src);
    GLuint fs=compile(GL_FRAGMENT_SHADER,frag_src);
    GLuint p=glCreateProgram();
    glAttachShader(p,vs); glAttachShader(p,fs);
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    printf("[2D] Shader linked\n");
    return p;
}

// ─── TEXTURE ─────────────────────────────────────────────────────────────────

static GLuint load_tex(const char *path){
    int w,h,n;
    unsigned char *d = stbi_load(path,&w,&h,&n,4);
    if(!d){ fprintf(stderr,"[2D] Cannot load %s\n",path); return 0; }
    GLuint t; glGenTextures(1,&t); glBindTexture(GL_TEXTURE_2D,t);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,d);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    stbi_image_free(d);
    printf("[2D] Texture loaded %dx%d\n",w,h);
    return t;
}

// ─── GRID INIT ───────────────────────────────────────────────────────────────

static void init_grid(){
    // Build base grid vertices covering NDC -0.9..0.9
    float x0=-0.82f, x1=0.82f;
    float y0=-0.98f, y1=0.98f;

    for(int r=0;r<=ROWS;r++){
        float vt = (float)r/ROWS;         // 0=top, 1=bottom in UV
        float ny = y1 + (y0-y1)*vt;      // NDC y: top=+, bottom=-
        for(int c=0;c<=COLS;c++){
            float us = (float)c/COLS;
            float nx = x0 + (x1-x0)*us;
            int i = r*(COLS+1)+c;
            base_x[i]=nx; base_y[i]=ny;
            uv_s[i]=us;   uv_t[i]=vt;
        }
    }

    // Build index buffer (two triangles per cell)
    int p=0;
    for(int r=0;r<ROWS;r++)
        for(int c=0;c<COLS;c++){
            int a=r*(COLS+1)+c, b=a+COLS+1;
            g_idx[p++]=a;   g_idx[p++]=b;   g_idx[p++]=a+1;
            g_idx[p++]=b;   g_idx[p++]=b+1; g_idx[p++]=a+1;
        }

    // Init current positions = base
    memcpy(cur_x,base_x,sizeof(cur_x));
    memcpy(cur_y,base_y,sizeof(cur_y));

    // Upload to GPU
    // Interleaved: x, y, u, v
    float *vdata = malloc(VERT_COUNT*4*sizeof(float));
    for(int i=0;i<VERT_COUNT;i++){
        vdata[i*4+0]=cur_x[i]; vdata[i*4+1]=cur_y[i];
        vdata[i*4+2]=uv_s[i];  vdata[i*4+3]=uv_t[i];
    }

    glGenVertexArrays(1,&g_vao); glBindVertexArray(g_vao);
    glGenBuffers(1,&g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER,g_vbo);
    glBufferData(GL_ARRAY_BUFFER,VERT_COUNT*4*sizeof(float),vdata,GL_DYNAMIC_DRAW);
    glGenBuffers(1,&g_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,g_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,IDX_COUNT*sizeof(unsigned),g_idx,GL_STATIC_DRAW);

    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);

    free(vdata);
    printf("[2D] Grid initialized: %d verts, %d indices\n",VERT_COUNT,IDX_COUNT);
}

// ─── SMOOTH VALUE ─────────────────────────────────────────────────────────────

static float smooth(float cur, float target, float speed){
    return cur + (target - cur) * speed;
}

// ─── ANIMATION STATE ─────────────────────────────────────────────────────────

static float s_mouth   = 0.0f;  // 0=closed 1=open
static float s_blink   = 0.0f;  // 0=open 1=closed
static float s_sway_x  = 0.0f;  // head sway left/right
static float s_sway_y  = 0.0f;  // head sway up/down
static float s_breath  = 0.0f;  // breathing scale

static float next_blink = 3.0f; // seconds until next blink
static float blink_timer = 0.0f;
static int   blinking = 0;
static float blink_t  = 0.0f;

// ─── UPDATE GRID ─────────────────────────────────────────────────────────────

static void update_grid(float dt, int mouth_target){
    // ── Smooth mouth
    float mouth_tgt = mouth_target ? 1.0f : 0.0f;
    s_mouth = smooth(s_mouth, mouth_tgt, 0.25f);

    // ── Auto blink
    blink_timer += dt;
    if(!blinking && blink_timer >= next_blink){
        blinking=1; blink_t=0.0f;
    }
    if(blinking){
        blink_t += dt * 8.0f;  // fast blink
        s_blink = sinf(blink_t * (float)M_PI);  // 0->1->0
        if(blink_t >= 1.0f){ blinking=0; blink_timer=0.0f;
                              next_blink=2.5f+((float)rand()/RAND_MAX)*3.0f;
                              s_blink=0.0f; }
    }

    // ── Head sway (subtle Lissajous)
    static float t = 0.0f; t += dt;
    float target_sx =  0.018f * sinf(t * 0.4f);
    float target_sy =  0.010f * sinf(t * 0.3f + 1.2f);
    s_sway_x = smooth(s_sway_x, target_sx, 0.05f);
    s_sway_y = smooth(s_sway_y, target_sy, 0.05f);

    // ── Breathing
    s_breath = 0.004f * sinf(t * 0.8f);

    // ── Apply to vertices
    for(int i=0;i<VERT_COUNT;i++){
        float us = uv_s[i];
        float vt = uv_t[i];

        float dx = 0.0f, dy = 0.0f;

        // Global sway
        dx += s_sway_x;
        dy += s_sway_y + s_breath;

        // ── Mouth deformation
        if(us>=MOUTH_U0 && us<=MOUTH_U1 && vt>=MOUTH_V0 && vt<=MOUTH_V1){
            // Weight: 1 at center of mouth, 0 at edges
            float wu = 1.0f - fabsf((us - (MOUTH_U0+MOUTH_U1)*0.5f) /
                                    ((MOUTH_U1-MOUTH_U0)*0.5f));
            float wv = (vt - MOUTH_V0) / (MOUTH_V1 - MOUTH_V0); // 0=top lip 1=chin
            float open_amt = s_mouth * 0.12f * wu;
            // Top lip moves up, bottom lip moves down
            if(wv < 0.5f)
                dy += open_amt * (0.5f - wv) * 2.0f;
            else
                dy -= open_amt * (wv - 0.5f) * 2.0f;
        }

        // ── Eye blink deformation
        // Left eye
        if(us>=LEYE_U0 && us<=LEYE_U1 && vt>=LEYE_V0 && vt<=LEYE_V1){
            float wu = 1.0f - fabsf((us-(LEYE_U0+LEYE_U1)*0.5f)/((LEYE_U1-LEYE_U0)*0.5f));
            float wv = 1.0f - fabsf((vt-(LEYE_V0+LEYE_V1)*0.5f)/((LEYE_V1-LEYE_V0)*0.5f));
            float w  = wu * wv * s_blink;
            // Squeeze vertically toward center
            float vc = (LEYE_V0+LEYE_V1)*0.5f;
            float ndc_scale = (1.65f); // approx NDC height per UV unit
            dy += (vc - vt) * w * 0.04f * ndc_scale;
        }
        // Right eye
        if(us>=REYE_U0 && us<=REYE_U1 && vt>=REYE_V0 && vt<=REYE_V1){
            float wu = 1.0f - fabsf((us-(REYE_U0+REYE_U1)*0.5f)/((REYE_U1-REYE_U0)*0.5f));
            float wv = 1.0f - fabsf((vt-(REYE_V0+REYE_V1)*0.5f)/((REYE_V1-REYE_V0)*0.5f));
            float w  = wu * wv * s_blink;
            float vc = (REYE_V0+REYE_V1)*0.5f;
            float ndc_scale = 1.65f;
            dy += (vc - vt) * w * 0.04f * ndc_scale;
        }

        cur_x[i] = base_x[i] + dx;
        cur_y[i] = base_y[i] + dy;
    }

    // Upload updated positions to GPU
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    // Update only x,y — stride 4 floats, only first 2
    // Easiest: rebuild interleaved and re-upload
    float *vdata = malloc(VERT_COUNT*4*sizeof(float));
    for(int i=0;i<VERT_COUNT;i++){
        vdata[i*4+0]=cur_x[i]; vdata[i*4+1]=cur_y[i];
        vdata[i*4+2]=uv_s[i];  vdata[i*4+3]=uv_t[i];
    }
    glBufferSubData(GL_ARRAY_BUFFER,0,VERT_COUNT*4*sizeof(float),vdata);
    free(vdata);
}

// ─── RENDER ──────────────────────────────────────────────────────────────────

static void render_face(){
    glUseProgram(g_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glUniform1i(glGetUniformLocation(g_prog,"tex"), 0);
    glBindVertexArray(g_vao);
    glDrawElements(GL_TRIANGLES, IDX_COUNT, GL_UNSIGNED_INT, 0);
}

// ─── FACE PROCESS ────────────────────────────────────────────────────────────

static void run_face_process(){
    printf("[2D] Starting SDL...\n");
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
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    setlocale(LC_NUMERIC,"C");
    printf("[2D] GL ready\n");

    g_prog = mkprog();
    init_grid();
    g_tex  = load_tex("face.png");

    Uint32 last = SDL_GetTicks();
    SDL_Event e;
    while(1){
        while(SDL_PollEvent(&e))
            if(e.type==SDL_QUIT) goto done;

        Uint32 now = SDL_GetTicks();
        float dt = (now - last) / 1000.0f;
        last = now;

        // Read mouth state
        int m = 0;
        FILE *f = fopen("/tmp/cloneme_mouth","r");
        if(f){ fscanf(f,"%d",&m); fclose(f); }

        update_grid(dt, m);

        glClearColor(0.13f, 0.13f, 0.16f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        render_face();
        SDL_GL_SwapWindow(win);
        SDL_Delay(16); // ~60fps
    }
done:
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    exit(0);
}

// ─── PUBLIC API ──────────────────────────────────────────────────────────────

void start_face_thread(){
    sem_init(&face_ready_sem,0,0);
#ifndef _WIN32
    face_pid = fork();
    if(face_pid == 0){
        int mx = sysconf(_SC_OPEN_MAX);
        for(int fd=3;fd<mx;fd++) close(fd);
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
    int w=0; for(int i=0;text[i];i++) if(text[i]==' ') w++;
    w++;
    for(int i=0;i<w*2;i++){
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
