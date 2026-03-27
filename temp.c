void render(int m, int is_talking) {
    // --- Time base for animations ---
    float t = SDL_GetTicks() * 0.001f;

    // --- Mouth morph (smooth open/close) ---
    static float s_mouth = 0.0f;
    float target = m ? 1.0f : 0.0f;   // m = speaking flag
    s_mouth += (target - s_mouth) * 0.2f;
    set_morph_weight(MOUTH_MORPH, s_mouth);

    // --- Eyelid blink (random interval) ---
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

    // --- Pupil dilation (oscillating) ---
    float pupil_val = 0.5f + 0.5f * sinf(t);
    set_morph_weight(PUPIL_MORPH, pupil_val);

    // --- Clamp weights for safety ---
    for(int i=0;i<morph_count;i++){
        if (morph_weights[i] < 0.0f) morph_weights[i] = 0.0f;
        if (morph_weights[i] > 1.0f) morph_weights[i] = 1.0f;
    }

    // --- Upload morphs independently ---
    upload_morph_uniforms(g_prog);
    glUniform1f(glGetUniformLocation(g_prog,"uMorph0"), morph_weights[MOUTH_MORPH]);
    glUniform1f(glGetUniformLocation(g_prog,"uMorph1"), morph_weights[EYELID_MORPH]);
    glUniform1f(glGetUniformLocation(g_prog,"uMorph2"), morph_weights[PUPIL_MORPH]);

    // --- Head idle animation ---
    if(g_head_vao && g_head_idx_count){
        M4 S, Rx, Ry, Rz, T, tmp, tmp2, tmp3, M_mat, MVP;
        mscl(S,   0.12f, 0.12f, 0.12f);

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
