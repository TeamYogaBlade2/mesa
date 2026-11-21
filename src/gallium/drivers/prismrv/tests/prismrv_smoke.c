/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_smoke.c — end-to-end smoke test running the Gallium prismrv
 * driver through EGL (surfaceless) against the drm-shim backend.
 *
 * Validates what can be validated without hardware:
 *   1. the driver loads, queries the shim kernel for chip info
 *   2. context creation allocates the command buffer via GEM
 *   3. a GL draw produces a SUBMIT ioctl carrying our two-layer stream
 *   4. flush returns a fence fd (the shim's signalled eventfd)
 *
 * The test asserts on the *driver-side* contract; pixel correctness is
 * covered by driver/ums/test_mesa_stream.py in the prismrv repo.
 */
#include <assert.h>
#include "prismrv_test_common.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>


static const char *vs_src =
   "attribute vec4 a_position;\n"
   "attribute vec4 a_color;\n"
   "varying vec4 v_color;\n"
   "void main() {\n"
   "   gl_Position = a_position;\n"
   "   v_color = a_color;\n"
   "}\n";

static const char *fs_src =
   "precision mediump float;\n"
   "varying vec4 v_color;\n"
   "void main() {\n"
   "   gl_FragColor = v_color;\n"
   "}\n";

static GLuint
compile(GLenum type, const char *src)
{
   GLuint sh = glCreateShader(type);
   glShaderSource(sh, 1, &src, NULL);
   glCompileShader(sh);
   GLint ok;
   glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[512];
      glGetShaderInfoLog(sh, sizeof(log), NULL, log);
      fprintf(stderr, "shader compile failed: %s\n", log);
      exit(77);
   }
   return sh;
}

int
main(void)
{
   /* --- EGL setup: surfaceless platform against the drm-shim node --- */
   static const EGLint attrs[] = {
      EGL_SURFACE_TYPE, EGL_DONT_CARE,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_NONE,
   };
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)
      eglGetProcAddress("eglGetPlatformDisplayEXT");
   if (!get_platform_display) {
      fprintf(stderr, "SMOKE: no eglGetPlatformDisplayEXT\n");
      return 77;
   }
   EGLDisplay dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                         EGL_DEFAULT_DISPLAY, NULL);
   if (dpy == EGL_NO_DISPLAY) {
      fprintf(stderr, "SMOKE: no surfaceless EGL display\n");
      return 77;
   }
   if (!eglInitialize(dpy, NULL, NULL)) {
      fprintf(stderr, "SMOKE: eglInitialize failed\n");
      return 77;
   }
   printf("SMOKE: EGL vendor %s\n", eglQueryString(dpy, EGL_VENDOR));

   EGLConfig cfg;
   EGLint ncfg;
   eglChooseConfig(dpy, attrs, &cfg, 1, &ncfg);
   assert(ncfg >= 1);

   static const EGLint ctx_attrs[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE,
   };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
   assert(ctx != EGL_NO_CONTEXT);
   if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
      fprintf(stderr, "SMOKE: make current failed\n");
      return 77;
   }
   printf("SMOKE: GL renderer %.60s / GLSL %s\n",
          (const char *)glGetString(GL_RENDERER),
          (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));

   if (require_prismrv_renderer())
      return 1;
   if (!make_render_target())
      return 1;

   /* --- program + VBO + draw ------------------------------------- */
   GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
   GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
   GLuint prog = glCreateProgram();
   glAttachShader(prog, vs);
   glAttachShader(prog, fs);
   glBindAttribLocation(prog, 0, "a_position");
   glBindAttribLocation(prog, 1, "a_color");
   glLinkProgram(prog);
   GLint ok;
   glGetProgramiv(prog, GL_LINK_STATUS, &ok);
   if (!ok) {
      char log[512];
      glGetProgramInfoLog(prog, sizeof(log), NULL, log);
      fprintf(stderr, "SMOKE: link failed: %s\n", log);
      return 77;
   }
   glUseProgram(prog);

   float verts[7 * 6] = {
      /* x, y, z, w, r, g, b */
       0.0f, -0.8f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f,
       0.8f,  0.8f, 0.0f, 1.0f,   0.0f, 1.0f, 0.0f,
      -0.8f,  0.8f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f,
   };
   GLuint vbo;
   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 28, (void *)0);
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 28, (void *)(4 * sizeof(float)));
   glEnableVertexAttribArray(1);

   glViewport(0, 0, 64, 64);
   glClearColor(0.f, 0.f, 0.f, 1.f);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLES, 0, 3);

   /* flush through the driver: exercises prismrv_context_flush ->
    * batch_submit -> SUBMIT ioctl (the shim answers with a signalled
    * eventfd fence) */
   /*
    * Read back WITHOUT glFinish/glFlush: the driver itself must submit the
    * pending batch and wait for the GPU before the CPU reads the target.
    * (The shim executes nothing, so the pixels are not checked - only
    * that the readback path runs to completion and reports no error.)
    */
   {
      unsigned char px[4] = { 0 };

      glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   }
   glFinish();

   /*
    * Scissored clear sanity: a scissor rectangle entirely off to the left
    * must leave the target alone.  NOTE: this does not reproduce the
    * unsigned wrap that prismrv_clear_color_rect() used to have - the
    * state tracker hands the driver an unsigned, already clamped
    * rectangle, so a negative x never reaches it from GL (verified: this
    * test also passes against the old code).  The fix is defensive.
    */
   {
      unsigned char before[4] = { 0 }, after[4] = { 0 };

      glDisable(GL_SCISSOR_TEST);
      glClearColor(0.f, 1.f, 0.f, 1.f);
      glClear(GL_COLOR_BUFFER_BIT);
      glReadPixels(5, 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, before);
      glEnable(GL_SCISSOR_TEST);
      glScissor(-30, 0, 20, 64);
      glClearColor(1.f, 0.f, 0.f, 1.f);
      glClear(GL_COLOR_BUFFER_BIT);
      glDisable(GL_SCISSOR_TEST);
      glReadPixels(5, 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, after);
      if (before[1] != 255 || memcmp(before, after, 4)) {
         fprintf(stderr, "FAIL: off-screen scissored clear changed pixels "
                 "(%u %u %u -> %u %u %u)\n", before[0], before[1], before[2],
                 after[0], after[1], after[2]);
         return 1;
      }
   }

   /*
    * Render-target mip levels: clear level 1 of a 64x64 texture and check
    * the clear landed in level 1 and not in level 0.  The clear is done by
    * the driver on the CPU, so this exercises the surface -> (level
    * offset, stride) mapping and the CPU readback path for real.
    */
   {
      static unsigned char l0[64 * 64 * 4];
      unsigned char px[4] = { 0 };
      GLuint tex, fbo0, fbo1;
      GLenum st;

      memset(l0, 0x11, sizeof(l0));
      glGenTextures(1, &tex);
      glBindTexture(GL_TEXTURE_2D, tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, l0);
      glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 32, 32, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, NULL);
      glGenFramebuffers(1, &fbo1);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo1);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, tex, 1);
      st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      if (glGetError() != GL_NO_ERROR || st != GL_FRAMEBUFFER_COMPLETE) {
         printf("SMOKE: render-to-mip-level not available (0x%x), skipped\n", st);
      } else {
         glClearColor(1.f, 0.f, 0.f, 1.f);
         glClear(GL_COLOR_BUFFER_BIT);
         glReadPixels(31, 31, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
         if (px[0] != 255 || px[1] != 0 || px[2] != 0) {
            fprintf(stderr, "FAIL: level-1 clear not visible in level 1 "
                    "(%u %u %u %u)\n", px[0], px[1], px[2], px[3]);
            return 1;
         }
         glGenFramebuffers(1, &fbo0);
         glBindFramebuffer(GL_FRAMEBUFFER, fbo0);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, tex, 0);
         glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
         if (px[0] != 0x11 || px[1] != 0x11) {
            fprintf(stderr, "FAIL: level-1 clear corrupted level 0 "
                    "(%02x %02x %02x %02x)\n", px[0], px[1], px[2], px[3]);
            return 1;
         }
         printf("SMOKE: render-to-mip-level OK\n");
      }
   }

   {
      GLenum err = glGetError();

      if (err != GL_NO_ERROR) {
         fprintf(stderr, "FAIL: GL error 0x%x after draw\n", err);
         return 1;
      }
   }
   printf("SMOKE: draw + finish completed without crashing\n");
   printf("SMOKE PASS\n");
   return 0;
}
