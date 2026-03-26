#include "face.h"
#include <SDL2/SDL.h>
#include <stdio.h>
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

// ─── DRAWING HELPERS ──────────────────────────────────────────────────────────

static void fill_ellipse(SDL_Renderer *ren, int cx, int cy, int rx, int ry) {
    for (int y = -ry; y <= ry; y++) {
        double t = (double)y / ry;
        int x = (int)(rx * sqrt(1.0 - t*t));
        SDL_RenderDrawLine(ren, cx-x, cy+y, cx+x, cy+y);
    }
}

static void fill_circle(SDL_Renderer *ren, int cx, int cy, int r) {
    fill_ellipse(ren, cx, cy, r, r);
}

static void draw_arc(SDL_Renderer *ren, int cx, int cy, int rx, int ry,
                     double a1, double a2, int steps) {
    double px = cx + rx * cos(a1), py = cy + ry * sin(a1);
    for (int i = 1; i <= steps; i++) {
        double a = a1 + (a2-a1)*i/steps;
        double nx = cx + rx*cos(a), ny = cy + ry*sin(a);
        SDL_RenderDrawLine(ren, (int)px, (int)py, (int)nx, (int)ny);
        px = nx; py = ny;
    }
}

// ─── DRAW FEMALE FACE ─────────────────────────────────────────────────────────

static void draw_face(SDL_Renderer *ren, int mouth) {
    // Background
    SDL_SetRenderDrawColor(ren, 240, 235, 228, 255);
    SDL_RenderClear(ren);

    // Shoulders
    SDL_SetRenderDrawColor(ren, 180, 130, 170, 255);
    fill_ellipse(ren, 100, 390, 110, 35);
    fill_ellipse(ren, 300, 390, 110, 35);
    SDL_Rect shoulders = {100, 370, 200, 50};
    SDL_RenderFillRect(ren, &shoulders);

    // Neck
    SDL_SetRenderDrawColor(ren, 245, 197, 163, 255);
    fill_ellipse(ren, 200, 340, 42, 28);
    SDL_Rect neck = {158, 328, 84, 55};
    SDL_RenderFillRect(ren, &neck);

    // Hair back layer
    SDL_SetRenderDrawColor(ren, 61, 31, 10, 255);
    fill_ellipse(ren, 200, 155, 155, 140);
    // Hair sides flowing down
    SDL_Rect hairL = {45, 155, 45, 220};
    SDL_RenderFillRect(ren, &hairL);
    fill_ellipse(ren, 67, 375, 22, 22);
    SDL_Rect hairR = {310, 155, 45, 220};
    SDL_RenderFillRect(ren, &hairR);
    fill_ellipse(ren, 333, 375, 22, 22);

    // Face base
    SDL_SetRenderDrawColor(ren, 245, 197, 163, 255);
    fill_ellipse(ren, 200, 210, 130, 158);
    fill_ellipse(ren, 200, 330, 100, 48);

    // Ears
    SDL_SetRenderDrawColor(ren, 240, 185, 144, 255);
    fill_ellipse(ren, 69, 220, 16, 23);
    fill_ellipse(ren, 331, 220, 16, 23);
    SDL_SetRenderDrawColor(ren, 245, 197, 163, 255);
    fill_ellipse(ren, 73, 220, 10, 16);
    fill_ellipse(ren, 327, 220, 10, 16);

    // Earrings
    SDL_SetRenderDrawColor(ren, 212, 175, 55, 255);
    fill_circle(ren, 69, 248, 5);
    fill_circle(ren, 331, 248, 5);

    // Hair top
    SDL_SetRenderDrawColor(ren, 74, 32, 16, 255);
    fill_ellipse(ren, 200, 90, 133, 82);
    SDL_SetRenderDrawColor(ren, 61, 31, 10, 255);
    fill_ellipse(ren, 200, 68, 108, 54);
    // Hair strands
    for (int t = 0; t < 4; t++) {
        draw_arc(ren, 200, 100+t, 118, 90, 3.5, 5.5, 30);
        draw_arc(ren, 200, 100+t, 118, 90, 3.8, 5.8, 30);
    }

    // Eyebrows — thin arched
    SDL_SetRenderDrawColor(ren, 61, 31, 10, 255);
    for (int t = 0; t < 3; t++) {
        draw_arc(ren, 148, 148+t, 28, 11, 3.5, 6.0, 20);
        draw_arc(ren, 252, 148+t, 28, 11, 3.14, 5.7, 20);
    }

    // Eyeshadow
    SDL_SetRenderDrawColor(ren, 180, 150, 200, 60);
    fill_ellipse(ren, 148, 164, 28, 10);
    fill_ellipse(ren, 252, 164, 28, 10);

    // Eye whites — almond shape
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    fill_ellipse(ren, 148, 175, 27, 17);
    fill_ellipse(ren, 252, 175, 27, 17);

    // Irises — green
    SDL_SetRenderDrawColor(ren, 60, 120, 80, 255);
    fill_circle(ren, 148, 175, 12);
    fill_circle(ren, 252, 175, 12);

    // Pupils
    SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
    fill_circle(ren, 148, 175, 6);
    fill_circle(ren, 252, 175, 6);

    // Eye highlights
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    fill_circle(ren, 152, 171, 3);
    fill_circle(ren, 256, 171, 3);

    // Eyelashes top
    SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
    for (int t = 0; t < 2; t++)
        draw_arc(ren, 148, 173+t, 28, 14, 3.4, 6.0, 20);
    for (int t = 0; t < 2; t++)
        draw_arc(ren, 252, 173+t, 28, 14, 3.14, 5.8, 20);

    // Nose — small feminine
    SDL_SetRenderDrawColor(ren, 220, 155, 105, 100);
    fill_ellipse(ren, 195, 226, 6, 5);
    fill_ellipse(ren, 205, 226, 6, 5);

    // Cheek blush
    SDL_SetRenderDrawColor(ren, 230, 120, 120, 55);
    fill_ellipse(ren, 118, 240, 32, 18);
    fill_ellipse(ren, 282, 240, 32, 18);

    if (!mouth) {
        // Closed — fuller feminine lips with cupid bow
        // Upper lip
        SDL_SetRenderDrawColor(ren, 180, 60, 90, 255);
        for (int t = 0; t < 4; t++)
            draw_arc(ren, 200, 278+t, 36, 12, 3.3, 6.1, 30);
        // Lower lip fuller
        SDL_SetRenderDrawColor(ren, 200, 80, 110, 255);
        for (int t = 0; t < 5; t++)
            draw_arc(ren, 200, 280+t, 36, 14, 0.1, 3.0, 30);
        // Lip line
        SDL_SetRenderDrawColor(ren, 150, 50, 75, 255);
        draw_arc(ren, 200, 278, 36, 10, 0.2, 2.9, 30);
        // Gloss
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 45);
        fill_ellipse(ren, 200, 278, 18, 5);
    } else {
        // Open mouth speaking
        SDL_SetRenderDrawColor(ren, 35, 12, 18, 255);
        fill_ellipse(ren, 200, 290, 32, 18);

        // Upper lip
        SDL_SetRenderDrawColor(ren, 180, 60, 90, 255);
        for (int t = 0; t < 4; t++)
            draw_arc(ren, 200, 276+t, 34, 10, 3.3, 6.1, 30);

        // Lower lip
        SDL_SetRenderDrawColor(ren, 200, 80, 110, 255);
        for (int t = 0; t < 4; t++)
            draw_arc(ren, 200, 292+t, 32, 14, 0.1, 3.0, 30);

        // Teeth
        SDL_SetRenderDrawColor(ren, 252, 248, 242, 255);
        fill_ellipse(ren, 200, 282, 24, 7);

        // Tongue
        SDL_SetRenderDrawColor(ren, 200, 80, 90, 255);
        fill_ellipse(ren, 200, 298, 16, 7);

        // Gloss
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 40);
        fill_ellipse(ren, 200, 278, 16, 4);
    }

    // Chin shadow
    SDL_SetRenderDrawColor(ren, 220, 160, 120, 50);
    fill_ellipse(ren, 200, 348, 62, 14);
}

// ─── FACE PROCESS ─────────────────────────────────────────────────────────────

static void run_face_process() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL Error: %s\n", SDL_GetError());
        exit(1);
    }

    SDL_Window   *win = SDL_CreateWindow("CloneMe",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           400, 430, SDL_WINDOW_SHOWN);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);

    int m = 0;
    SDL_Event e;
    while (1) {
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT) goto cleanup;

        FILE *f = fopen("/tmp/cloneme_mouth", "r");
        if (f) { fscanf(f, "%d", &m); fclose(f); }

        draw_face(ren, m);
        SDL_RenderPresent(ren);
        SDL_Delay(50);
    }
cleanup:
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    exit(0);
}

// ─── PUBLIC API ───────────────────────────────────────────────────────────────

void start_face_thread() {
    sem_init(&face_ready_sem, 0, 0);
#ifndef _WIN32
    face_pid = fork();
    if (face_pid == 0) {
        int maxfd = sysconf(_SC_OPEN_MAX);
        for (int fd = 3; fd < maxfd; fd++) close(fd);
        run_face_process();
        exit(0);
    }
#endif
    sem_post(&face_ready_sem);
}

void stop_face_thread() {
#ifndef _WIN32
    if (face_pid > 0) {
        kill(face_pid, SIGTERM);
        waitpid(face_pid, NULL, 0);
        face_pid = -1;
    }
#endif
}

void animate_mouth_for(const char *text) {
    int words = 0;
    for (int i = 0; text[i]; i++)
        if (text[i] == ' ') words++;
    words++;
    int frames = (words * 200) / 100;
    for (int i = 0; i < frames; i++) {
        mouth_open = (i % 2);
        FILE *f = fopen("/tmp/cloneme_mouth", "w");
        if (f) { fprintf(f, "%d", mouth_open); fclose(f); }
        usleep(100000);
    }
    mouth_open = 0;
    FILE *f = fopen("/tmp/cloneme_mouth", "w");
    if (f) { fprintf(f, "0"); fclose(f); }
}

void start_mouth_animation(void) {
    mouth_open = 1;
    FILE *f = fopen("/tmp/cloneme_mouth", "w");
    if (f) { fprintf(f, "1"); fclose(f); }
}

void stop_mouth_animation(void) {
    mouth_open = 0;
    FILE *f = fopen("/tmp/cloneme_mouth", "w");
    if (f) { fprintf(f, "0"); fclose(f); }
}
