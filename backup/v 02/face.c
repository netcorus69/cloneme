#include "face.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <semaphore.h>
#include <math.h>
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

static void fill_circle(SDL_Renderer *ren, int cx, int cy, int r) {
    for (int y = -r; y <= r; y++) {
        int x = (int)sqrt((double)(r*r - y*y));
        SDL_RenderDrawLine(ren, cx - x, cy + y, cx + x, cy + y);
    }
}

static void fill_ellipse(SDL_Renderer *ren, int cx, int cy, int rx, int ry) {
    for (int y = -ry; y <= ry; y++) {
        double t = (double)y / ry;
        int x = (int)(rx * sqrt(1.0 - t*t));
        SDL_RenderDrawLine(ren, cx - x, cy + y, cx + x, cy + y);
    }
}

static void draw_arc(SDL_Renderer *ren, int cx, int cy, int rx, int ry,
                     double a1, double a2, int steps) {
    double prev_x = cx + rx * cos(a1);
    double prev_y = cy + ry * sin(a1);
    for (int i = 1; i <= steps; i++) {
        double a = a1 + (a2 - a1) * i / steps;
        double nx = cx + rx * cos(a);
        double ny = cy + ry * sin(a);
        SDL_RenderDrawLine(ren, (int)prev_x, (int)prev_y, (int)nx, (int)ny);
        prev_x = nx;
        prev_y = ny;
    }
}

// ─── DRAW HUMAN FACE ──────────────────────────────────────────────────────────

static void draw_face(SDL_Renderer *ren, int mouth) {
    // Background
    SDL_SetRenderDrawColor(ren, 240, 235, 220, 255);
    SDL_RenderClear(ren);

    // Neck - centered below face
    SDL_SetRenderDrawColor(ren, 245, 197, 163, 255);
    fill_ellipse(ren, 200, 355, 45, 30);
    SDL_Rect neck = {155, 340, 90, 70};
    SDL_RenderFillRect(ren, &neck);

    // Ears
    SDL_SetRenderDrawColor(ren, 240, 185, 144, 255);
    fill_ellipse(ren, 90, 190, 18, 26);
    fill_ellipse(ren, 310, 190, 18, 26);

    // Face base
    SDL_SetRenderDrawColor(ren, 245, 197, 163, 255);
    fill_ellipse(ren, 200, 190, 110, 145);

    // Hair
    SDL_SetRenderDrawColor(ren, 58, 36, 18, 255);
    fill_ellipse(ren, 200, 80, 112, 75);
    fill_ellipse(ren, 200, 60, 90, 50);
    SDL_Rect hairL = {88, 80, 30, 70};
    SDL_RenderFillRect(ren, &hairL);
    SDL_Rect hairR = {282, 80, 30, 70};
    SDL_RenderFillRect(ren, &hairR);

    // Eyebrows
    SDL_SetRenderDrawColor(ren, 58, 36, 18, 255);
    for (int t = 0; t < 4; t++)
        draw_arc(ren, 148, 128+t, 30, 12, 3.4, 6.0, 20);
    for (int t = 0; t < 4; t++)
        draw_arc(ren, 252, 128+t, 30, 12, 3.14, 5.8, 20);

    // Eye whites
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    fill_ellipse(ren, 148, 162, 28, 18);
    fill_ellipse(ren, 252, 162, 28, 18);

    // Irises
    SDL_SetRenderDrawColor(ren, 80, 55, 40, 255);
    fill_circle(ren, 148, 162, 12);
    fill_circle(ren, 252, 162, 12);

    // Pupils
    SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
    fill_circle(ren, 148, 162, 6);
    fill_circle(ren, 252, 162, 6);

    // Eye highlights
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    fill_circle(ren, 152, 158, 3);
    fill_circle(ren, 256, 158, 3);

    // Nose
    SDL_SetRenderDrawColor(ren, 220, 160, 110, 255);
    fill_ellipse(ren, 193, 218, 8, 6);
    fill_ellipse(ren, 207, 218, 8, 6);

    // Cheek blush
    SDL_SetRenderDrawColor(ren, 230, 160, 140, 60);
    fill_ellipse(ren, 120, 210, 28, 16);
    fill_ellipse(ren, 280, 210, 28, 16);

    if (!mouth) {
        // Closed mouth — subtle smile
        SDL_SetRenderDrawColor(ren, 180, 100, 80, 255);
        for (int t = 0; t < 3; t++)
            draw_arc(ren, 200, 255+t, 38, 10, 0.2, 2.9, 30);
        // Lip line
        SDL_SetRenderDrawColor(ren, 192, 112, 96, 255);
        draw_arc(ren, 200, 252, 38, 8, 0.15, 2.98, 30);
    } else {
        // Open mouth — speaking
        // Dark interior
        SDL_SetRenderDrawColor(ren, 40, 15, 15, 255);
        fill_ellipse(ren, 200, 268, 36, 20);

        // Upper lip
        SDL_SetRenderDrawColor(ren, 192, 112, 96, 255);
        for (int t = 0; t < 4; t++)
            draw_arc(ren, 200, 253+t, 36, 10, 3.3, 6.1, 30);

        // Lower lip
        SDL_SetRenderDrawColor(ren, 210, 128, 112, 255);
        for (int t = 0; t < 4; t++)
            draw_arc(ren, 200, 268+t, 36, 14, 0.1, 3.0, 30);

        // Teeth
        SDL_SetRenderDrawColor(ren, 250, 248, 240, 255);
        fill_ellipse(ren, 200, 258, 28, 8);

        // Tongue
        SDL_SetRenderDrawColor(ren, 200, 80, 80, 255);
        fill_ellipse(ren, 200, 278, 20, 8);
    }
}

// ─── FACE PROCESS ─────────────────────────────────────────────────────────────

static void run_face_process() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL Error: %s\n", SDL_GetError());
        exit(1);
    }

    SDL_Window   *win = SDL_CreateWindow("CloneMe",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           400, 420, SDL_WINDOW_SHOWN);
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
