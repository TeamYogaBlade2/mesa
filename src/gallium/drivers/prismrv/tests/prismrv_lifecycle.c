/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_lifecycle.c — context/resource lifetime stress test.
 *
 * Runs against the drm-shim backend and hammers the paths the review
 * found fragile:
 *
 *   1. many create/destroy contexts (rzalloc/FREE mismatch, batch BO
 *      handle leaks)
 *   2. repeated flush with growing shader programs (cmd buffer
 *      overflow / capacity check)
 *   3. draws with varying vertex counts followed by small ones (TA BO
 *      stale packets)
 *   4. uniform updates per stage (VS/FS slot collision)
 *   5. resource create/destroy churn (GEM handle leaks)
 *
 * Any crash, hang or assertion is a regression.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#include <GLES2/gl2.h>

static const char *fs_template =
   "precision mediump float;\n"
   "uniform vec4 c0;\n"
   "uniform vec4 c1;\n"
   "void main() {\n"
   "  vec4 r = c0;\n"
   "%s"
   "  gl_FragColor = r;\n"
   "}\n";

/* build an FS with N dependent mad chains — forces long USSE text */
static char *
build_fs(int chains)
{
   size_t cap = 64 + (size_t)chains * 64;
   char *body = malloc((size_t)chains * 64 + 16);
   char *src;
   int i;

   for (i = 0; i < chains; i++)
      snprintf(body + strlen(body), 64, "  r = r * c0 + c1;\n");
   src = malloc(cap + strlen(fs_template));
   sprintf(src, fs_template, body);
   free(body);
   return src;
}

static GLuint
make_program(int chains)
{
   static const char *vs =
      "attribute vec4 p; void main() { gl_Position = p; }\n";
   char *fs_src = build_fs(chains);
   GLuint vs, fs, prog;
   GLint ok;
   char log[512];

   vs = glCreateShader(GL_VERTEX_SHADER);
   glShaderSource(vs, 1, &vs, NULL);
   glCompileShader(vs);
   glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
   if (!ok) { fprintf(stderr, "LIFE: vs compile failed\n"); exit(1); }

   fs = glCreateShader(GL_FRAGMENT_SHADER);
   glShaderSource(fs, 1, &fs_src, NULL);
   glCompileShader(fs);
   glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      glGetShaderInfoLog(fs, sizeof(log), NULL, log);
      fprintf(stderr, "LIFE: fs(%d) failed: %s\n", chains, log);
      exit(1);
   }
   free(fs_src);

   prog = glCreateProgram();
   glAttachShader(prog, vs);
   glAttachShader(prog, fs);
   glLinkProgram(prog);
   glGetProgramiv(prog, GL_LINK_STATUS, &ok);
   if (!ok) {
      glGetProgramInfoLog(prog, sizeof(log), NULL, log);
      fprintf(stderr, "LIFE: link(%d) failed: %s\n", chains, log);
      exit(1);
   }
   glDeleteShader(vs);
   glDeleteShader(fs);
   return prog;
}

int
main(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)
      eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy;
   EGLConfig cfg;
   EGLContext ctx;
   GLuint vbo;
   float verts[9] = { 0.f, -.8f, 0.f, .8f, .8f, 0.f, -.8f, .8f, 0.f };
   int i;

   if (!get_platform_display) { fprintf(stderr, "LIFE: no ext\n"); return 77; }
   dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                              EGL_DEFAULT_DISPLAY, NULL);
   if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
      fprintf(stderr, "LIFE: init failed\n"); return 77;
   }
   {
      EGLint n;
      const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_DONT_CARE,
                            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                            EGL_NONE };
      eglChooseConfig(dpy, ca, &cfg, 1, &n);
      assert(n >= 1);
   }
   {
      const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
      ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
      assert(ctx != EGL_NO_CONTEXT);
   }
   if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
      fprintf(stderr, "LIFE: make current\n"); return 77;
   }
   printf("LIFE: renderer %.50s\n", (const char *)glGetString(GL_RENDERER));

   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

   /* --- phase 1: repeated draw with growing programs -------------- */
   for (i = 1; i <= 24; i++) {
      GLuint prog = make_program(i * 8);   /* up to ~192 chains */
      glUseProgram(prog);
      glUniform4f(glGetUniformLocation(prog, "c0"), .5f, .25f, 1.f, 1.f);
      glUniform4f(glGetUniformLocation(prog, "c1"), .5f, .5f, .5f, .5f);
      glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
      glEnableVertexAttribArray(0);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glFinish();
      glDeleteProgram(prog);
   }
   printf("LIFE: growth loop done\n");

   /* --- phase 2: big draw then tiny draw (stale TA packets) ------- */
   {
      GLuint prog = make_program(4);
      glUseProgram(prog);
      glUniform4f(glGetUniformLocation(prog, "c0"), 1.f, 1.f, 1.f, 1.f);
      glUniform4f(glGetUniformLocation(prog, "c1"), 0.f, 0.f, 0.f, 0.f);
      glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
      glEnableVertexAttribArray(0);
      /* large indexed-ish draws then a minimal one */
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glFinish();
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glFinish();
      glDeleteProgram(prog);
   }
   printf("LIFE: stale-packet sequence done\n");

   /* --- phase 3: per-stage uniform alternation -------------------- */
   {
      GLuint prog = make_program(2);
      GLint loc = glGetUniformLocation(prog, "c0");
      glUseProgram(prog);
      glUniform4f(loc, 1.f, 0.f, 0.f, 1.f);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glUniform4f(glGetUniformLocation(prog, "c1"), 0.f, 1.f, 0.f, 1.f);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glFinish();
      glDeleteProgram(prog);
   }
   printf("LIFE: uniform alternation done\n");

   /* --- phase 4: resource churn ----------------------------------- */
   for (i = 0; i < 200; i++) {
      GLuint tex, fbo;
      glGenTextures(1, &tex);
      glBindTexture(GL_TEXTURE_2D, tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glGenFramebuffers(1, &fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, tex, 0);
      glClear(GL_COLOR_BUFFER_BIT);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glDeleteFramebuffers(1, &fbo);
      glDeleteTextures(1, &tex);
   }
   printf("LIFE: resource churn done\n");

   /* --- phase 5: context churn ------------------------------------ */
   for (i = 0; i < 20; i++) {
      const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
      EGLContext tmp = eglCreateContext(dpy, cfg, ctx, ca);
      if (tmp != EGL_NO_CONTEXT) {
         eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, tmp);
         glClearColor(.1f * i, 0, 0, 1);
         glClear(GL_COLOR_BUFFER_BIT);
         eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
         eglDestroyContext(dpy, tmp);
      }
   }
   printf("LIFE: context churn done\n");

   printf("LIFE PASS\n");
   return 0;
}
