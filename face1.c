#include "face.h"
#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <semaphore.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

static pid_t face_pid = -1;
int mouth_open = 0;
sem_t face_ready_sem;

// ─── SHADERS ──────────────────────────────────────────────────────────────────

static const char *vert_src =
"#version 330 core\n"
"layout(location=0) in vec3 aPos;\n"
"layout(location=1) in vec3 aNorm;\n"
"out vec3 vN; out vec3 vP;\n"
"uniform mat4 MVP; uniform mat4 M;\n"
"void main(){\n"
"  vP=vec3(M*vec4(aPos,1)); vN=normalize(mat3(M)*aNorm);\n"
"  gl_Position=MVP*vec4(aPos,1);\n"
"}\n";

// Realistic Blinn-Phong fragment shader
static const char *frag_src =
"#version 330 core\n"
"in vec3 vN; in vec3 vP;\n"
"out vec4 C;\n"
"uniform vec3 col;            // base color (albedo)\n"
"uniform vec3 lightPos;       // world-space light position\n"
"uniform vec3 viewPos;        // world-space camera position\n"
"uniform float shininess;     // specular exponent\n" 
"uniform float specStrength;  // specular intensity\n"
"uniform float sp;            // kept for compatibility (unused here)\n"
"void main(){\n"
"  vec3 N = normalize(vN);\n"
"  vec3 L = normalize(lightPos - vP);\n"
"  vec3 V = normalize(viewPos - vP);\n"
"  vec3 H = normalize(L + V);\n"
"\n"
"  // Ambient (soft warm ambient to hint subsurface)\n"
"  vec3 ambient = 0.18 * col + vec3(0.02, 0.01, 0.01);\n"
"\n"
"  // Diffuse (Lambert)\n"
"  float diff = max(dot(N, L), 0.0);\n"
"\n"
"  // Specular (Blinn-Phong)\n"
"  float spec = 0.0;\n"
"  if(diff > 0.0) spec = pow(max(dot(N, H), 0.0), shininess) * specStrength;\n"
"\n"
"  // Slight rim to keep face readable\n"
"  float rim = pow(1.0 - max(dot(N, V), 0.0), 2.5) * 0.06;\n"
"\n"
"  vec3 color = ambient + diff * col + spec * vec3(1.0) + rim * col;\n"
"  // gamma correct-ish clamp\n"
"  color = clamp(color, 0.0, 1.0);\n"
"  C = vec4(color, 1.0);\n"
"}\n";

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

static GLuint mkprog(){
    auto GLuint mks(GLenum t,const char*s){
        GLuint id=glCreateShader(t);
        glShaderSource(id,1,&s,NULL);glCompileShader(id);
        GLint ok;glGetShaderiv(id,GL_COMPILE_STATUS,&ok);
        if(!ok){char b[512];glGetShaderInfoLog(id,512,NULL,b);fprintf(stderr,"SH:%s\n",b);}
        return id;
    }
    GLuint vs=mks(GL_VERTEX_SHADER,vert_src),fs=mks(GL_FRAGMENT_SHADER,frag_src);
    GLuint p=glCreateProgram();
    glAttachShader(p,vs);glAttachShader(p,fs);glLinkProgram(p);
    glDeleteShader(vs);glDeleteShader(fs);return p;
}

// ─── ANIMATION ───────────────────────────────────────────────────────────────

static float mr=0,bl=0,bd=0,hs=0,sm=0,hb=0;
static int bt=120;

// ─── RENDER ───────────────────────────────────────────────────────────────────

static void render(int m, int tick){
    // Smooth animations
    mr+=((m?1.f:0.f)-mr)*0.14f;
    hs=m?sinf(tick*0.13f)*0.025f:hs*0.94f;
    hb=m?sinf(tick*0.17f)*0.018f:hb*0.94f;
    sm+=((m?1.f:0.f)-sm)*0.05f;
    bt--;
    if(bt<=0&&bd==0.f)bd=1.f;
    if(bd>0.f){bl+=0.28f;if(bl>=1.f){bl=1.f;bd=-1.f;}}
    if(bd<0.f){bl-=0.28f;if(bl<=0.f){bl=0.f;bd=0.f;bt=100+rand()%80;}}
    float H=hb;

    // Set lighting uniforms for Blinn-Phong shader
    // light position and view position are in world space (match view matrix used)
    glUseProgram(g_prog);
    GLint loc_light = glGetUniformLocation(g_prog, "lightPos");
    GLint loc_view  = glGetUniformLocation(g_prog, "viewPos");
    GLint loc_shin  = glGetUniformLocation(g_prog, "shininess");
    GLint loc_spec  = glGetUniformLocation(g_prog, "specStrength");
    if(loc_light>=0) glUniform3f(loc_light, 1.8f, 2.2f, 3.5f);
    if(loc_view>=0)  glUniform3f(loc_view, 0.0f, 0.0f, 4.5f);
    if(loc_shin>=0)  glUniform1f(loc_shin, 32.0f);
    if(loc_spec>=0)  glUniform1f(loc_spec, 0.45f);

    // Cartoon skin — warmer, more saturated than realistic (base tones still used)
    #define SK  0.972f,0.820f,0.690f
    #define SKD 0.900f,0.740f,0.600f
    #define SKL 0.990f,0.880f,0.780f
    #define HR  0.200f,0.105f,0.035f   // dark brown
    #define HRL 0.380f,0.220f,0.090f   // highlight

    // ── Shoulders ──
    dp(-1.05f,-1.52f+H,-.15f, .60f,.24f,.55f, .50f,.28f,.55f,.1f);
    dp( 1.05f,-1.52f+H,-.15f, .60f,.24f,.55f, .50f,.28f,.55f,.1f);
    dp(    0.0f,-1.58f+H,-.10f, .82f,.18f,.50f, .48f,.26f,.52f,.1f);

    // ── Neck ──
    dp(0,-1.08f+H,.06f, .17f,.32f,.17f, SKD,.1f);

    // ── Hair back ──
    dp(hs,      .00f+H,-.32f, .82f,1.08f,.40f, HR,.2f);
    dp(-.82f+hs,.00f+H,-.10f, .15f,1.12f,.28f, HR,.2f);
    dp( .82f+hs,.00f+H,-.10f, .15f,1.12f,.28f, HR,.2f);

    // ── HEAD — big cartoon oval, taller than wide ──
    dp(0,.05f+H,0, .68f,1.05f,.72f, SK,.08f);

    // ── Forehead ──
    dp(0,.52f+H,.26f, .52f,.38f,.30f, SKL,.10f);

    // ── Hair top ──
    dp(hs,     .96f+H,.08f, .72f,.42f,.72f, HR,.22f);
    dp(hs*.5f, .82f+H,-.38f, .62f,.28f,.36f, HR,.18f);
    // Shine streak
    dp(-.08f+hs,.92f+H,.42f, .18f,.11f,.18f, HRL,.55f);

    // ── Ears ──
    dp(-.70f,.02f+H,.0f, .06f,.11f,.05f, SKD,.1f);
    dp( .70f,.02f+H,.0f, .06f,.11f,.05f, SKD,.1f);
    dp(-.71f,-.14f+H,.0f, .030f,.030f,.030f, .92f,.78f,.10f,1.f);
    dp( .71f,-.14f+H,.0f, .030f,.030f,.030f, .92f,.78f,.10f,1.f);

    // ── Cartoon eyebrows — thick expressive ──
    float brow_y = sm * 0.025f;  // raise when smiling
    dp(-.270f,.430f+H+brow_y,.622f, .145f,.025f,.038f, .15f,.08f,.02f,.1f);
    dp( .270f,.430f+H+brow_y,.622f, .145f,.025f,.038f, .15f,.08f,.02f,.1f);

    // ── Big cartoon eyes — Pixar style ──
    float ey=bl*.095f;
    // White sclera — large
    dp(-.268f,.240f+H,.640f, .128f,.105f-ey,.095f, .99f,.99f,.99f,.15f);
    dp( .268f,.240f+H,.640f, .128f,.105f-ey,.095f, .99f,.99f,.99f,.15f);
    // Coloured iris — large and vivid
    dp(-.268f,.238f+H,.688f, .082f,.090f-ey,.065f, .12f,.52f,.28f,.60f);
    dp( .268f,.238f+H,.688f, .082f,.090f-ey,.065f, .12f,.52f,.28f,.60f);
    // Dark ring around iris
    dp(-.268f,.238f+H,.690f, .086f,.094f-ey,.062f, .05f,.22f,.12f,.20f);
    dp( .268f,.238f+H,.690f, .086f,.094f-ey,.062f, .05f,.22f,.12f,.20f);
    // Pupil
    dp(-.268f,.235f+H,.715f, .042f,.048f-ey,.038f, .02f,.02f,.02f,.9f);
    dp( .268f,.235f+H,.715f, .042f,.048f-ey,.038f, .02f,.02f,.02f,.9f);
    // Eye shine — cartoon highlight
    dp(-.252f,.258f+H,.728f, .022f,.022f,.015f, .98f,.98f,.98f,.0f);
    dp( .284f,.258f+H,.728f, .022f,.022f,.015f, .98f,.98f,.98f,.0f);
    dp( .268f,.258f+H,.728f, .013f,.013f,.010f, .90f,.90f,.90f,.0f);
    dp(-.268f,.258f+H,.728f, .013f,.013f,.010f, .90f,.90f,.90f,.0f);
    // Upper eyelid
    dp(-.268f,.308f+bl*.100f+H,.658f, .138f,.032f,.068f, SK,.06f);
    dp( .268f,.308f+bl*.100f+H,.658f, .138f,.032f,.068f, SK,.06f);
    // Lower eyelid subtle
    dp(-.268f,.175f+H,.655f, .122f,.020f,.060f, SKD,.04f);
    dp( .268f,.175f+H,.655f, .122f,.020f,.060f, SKD,.04f);
    // Eyeshadow
    dp(-.268f,.358f+H,.615f, .125f,.018f,.032f, .55f,.38f,.65f,.05f);
    dp( .268f,.358f+H,.615f, .125f,.018f,.032f, .55f,.38f,.65f,.05f);

    // ── Nose — cartoon: tiny button nose ──
    dp(0,-.02f+H,.715f, .062f,.055f,.058f, SKD,.15f);
    // Tiny nostril dots
    dp(-.038f,-.052f+H,.705f, .020f,.016f,.020f, .82f,.64f,.50f,.05f);
    dp( .038f,-.052f+H,.705f, .020f,.016f,.020f, .82f,.64f,.50f,.05f);

    // ── Cartoon mouth — wide smile ──
    float lop=mr*.085f, smy=sm*.035f;
    // Upper lip
    dp(0,-.310f+H+smy,.705f, .200f,.038f+lop,.060f, .72f,.24f,.38f,.35f);
    // Lower lip — bigger for cartoon
    dp(0,-.398f-lop+H+smy,.700f, .185f,.060f+lop,.065f, .78f,.30f,.45f,.45f);
    // Lip line
    dp(0,-.356f+H+smy,.712f, .178f,.015f,.042f, .58f,.16f,.28f,.15f);
    // Mouth corners — cute upward curve
    dp(-.168f,-.338f+H+smy,.702f, .028f,.025f,.035f, .65f,.20f,.35f,.20f);
    dp( .168f,-.338f+H+smy,.702f, .028f,.025f,.035f, .65f,.20f,.35f,.20f);
    // Teeth
    if(mr>.18f)
        dp(0,-.352f+H+smy,.708f, .130f*mr,.028f*mr,.035f, .97f,.95f,.92f,.35f);

    // ── Chin ──
    dp(0,-.730f+H,.400f, .280f,.195f,.230f, SKD,.06f);

    // ── Cheek blush — cartoon style, subtle ──
    dp(-.360f,.060f+H,.580f, .140f,.090f,.065f, .98f,.76f,.72f,.02f);
    dp( .360f,.060f+H,.580f, .140f,.090f,.065f, .98f,.76f,.72f,.02f);
}

// ─── FACE PROCESS ─────────────────────────────────────────────────────────────

static void run_face_process(){
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

    g_prog=mkprog(); init_sphere(); glUseProgram(g_prog);

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
