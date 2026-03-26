#include "face.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <pthread.h>
#endif

static SDL_Renderer *ren = NULL;
static SDL_Window *win = NULL;
static int running = 1;
int mouth_open = 0; // shared state

// Draw the face with current mouth state
static void draw_face() {
    if (!ren) return;

    SDL_SetRenderDrawColor(ren, 255, 255, 0, 255);
    SDL_RenderClear(ren);

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_Rect leftEye = {100, 100, 50, 50};
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
        printf("SDL Init Error: %s\n", SDL_GetError());
        return;
    }
    win = SDL_CreateWindow("Clone Face",
                           SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED,
                           400, 400,
                           SDL_WINDOW_SHOWN);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);

    SDL_Event e;
    while (running) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
        }
        draw_face();
        SDL_Delay(50); // refresh ~20 FPS
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
}

#ifdef _WIN32
DWORD WINAPI face_thread(LPVOID lpParam) {
    face_loop();
    return 0;
}
#else
void *face_thread(void *arg) {
    face_loop();
    return NULL;
}
#endif

void start_face_thread() {
#ifdef _WIN32
    CreateThread(NULL, 0, face_thread, NULL, 0, NULL);
#else
    pthread_t tid;
    pthread_create(&tid, NULL, face_thread, NULL);
#endif
}

// Animate mouth for estimated speech duration
void animate_mouth_for(const char *text) {
    // Estimate duration based on word count
    int words = 0;
    for (int i = 0; text[i]; i++) {
        if (text[i] == ' ') words++;
    }
    words++; // last word

    int duration_ms = words * 400; // ~400ms per word
    int frames = duration_ms / 200; // 200ms per frame

    for (int i = 0; i < frames; i++) {
        mouth_open = (i % 2);
#ifdef _WIN32
        Sleep(200);
#else
        usleep(200000);
#endif
    }
    mouth_open = 0; // reset closed
}

void stop_face_thread() {
    running = 0;
}
