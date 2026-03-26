#include "face.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <semaphore.h>   // ✅ added for sem_post

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <pthread.h>
#endif

static SDL_Renderer *ren = NULL;
static SDL_Window   *win = NULL;
static int running = 1;

int mouth_open = 0; // shared state

// ✅ Defined here — cloneme.c uses `extern sem_t face_ready_sem` to reference it
sem_t face_ready_sem;

// Draw the face with current mouth state
static void draw_face() {
    if (!ren) return;

    SDL_SetRenderDrawColor(ren, 255, 255, 0, 255);
    SDL_RenderClear(ren);

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_Rect leftEye  = {100, 100, 50, 50};
    SDL_Rect rightEye = {250, 100, 50, 50};
    SDL_RenderFillRect(ren, &leftEye);
    SDL_RenderFillRect(ren, &rightEye);

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    if (mouth_open) {
        SDL_Rect mouth = {150, 240, 100, 40};
        SDL_RenderFillRect(ren, &mouth);
    } else {
        SDL_RenderDrawLine(ren, 100, 250, 300, 250);
    }

    SDL_RenderPresent(ren);
}

// SDL loop runs in its own thread
static void face_loop() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "❌ SDL Init Error: %s\n", SDL_GetError());
        sem_post(&face_ready_sem); // ✅ unblock main even on failure so it doesn't hang
        return;
    }

    win = SDL_CreateWindow("Clone Face",
                           SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED,
                           400, 400,
                           SDL_WINDOW_SHOWN);

    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);

    // ✅ Signal main() that the face window is ready — replaces sleep(5)
    sem_post(&face_ready_sem);

    SDL_Event e;
    while (running) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
        }
        draw_face();
        SDL_Delay(50); // ~20 FPS
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
}

#ifdef _WIN32
DWORD WINAPI face_thread(LPVOID lpParam) {
    (void)lpParam;
    face_loop();
    return 0;
}
#else
void *face_thread(void *arg) {
    (void)arg;
    face_loop();
    return NULL;
}
#endif

void start_face_thread() {
    // ✅ Initialize semaphore to 0 before spawning the thread
    sem_init(&face_ready_sem, 0, 0);

#ifdef _WIN32
    CreateThread(NULL, 0, face_thread, NULL, 0, NULL);
#else
    pthread_t tid;
    pthread_create(&tid, NULL, face_thread, NULL);
#endif
}

// Animate mouth for estimated speech duration
void animate_mouth_for(const char *text) {
    int words = 0;
    for (int i = 0; text[i]; i++) {
        if (text[i] == ' ') words++;
    }
    words++;                        // count last word
    int duration_ms = words * 200;  // ~200ms per word
    int frames = duration_ms / 100;

    for (int i = 0; i < frames; i++) {
        mouth_open = (i % 2);
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
    }
    mouth_open = 0; // reset to closed
}

void stop_face_thread() {
    running = 0;
}

void start_mouth_animation(void) {
    mouth_open = 1;
}

void stop_mouth_animation(void) {
    mouth_open = 0;
}
