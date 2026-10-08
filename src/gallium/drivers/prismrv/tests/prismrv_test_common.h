/*
 * Shared helpers for the prismrv EGL tests.
 *
 * require_prismrv_renderer(): the tests used to pass silently on
 * llvmpipe when the loader did not pick this driver.  Fail instead.
 * make_render_target(): the surfaceless default framebuffer is
 * incomplete, so every clear/draw was rejected by Mesa before reaching
 * the driver.  Render into a 64x64 RGBA texture instead.
 */
#ifndef PRISMRV_TEST_COMMON_H
#define PRISMRV_TEST_COMMON_H

#include <stdio.h>
#include <string.h>
#include <GLES2/gl2.h>

static inline int
require_prismrv_renderer(void)
{
   const char *r = (const char *)glGetString(GL_RENDERER);

   if (!r || !strstr(r, "sgx544")) {
      fprintf(stderr, "FAIL: GL_RENDERER is '%s', not the prismrv driver\n",
              r ? r : "(null)");
      return 1;
   }
   return 0;
}

static inline GLuint
make_render_target(void)
{
   GLuint tex, fbo;

   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, NULL);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                          GL_TEXTURE_2D, tex, 0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "FAIL: render-target framebuffer incomplete (0x%x)\n",
              glCheckFramebufferStatus(GL_FRAMEBUFFER));
      return 0;
   }
   return fbo;
}

#endif
