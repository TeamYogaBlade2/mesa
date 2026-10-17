#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
static int nfds(void){int n=0;DIR*d=opendir("/proc/self/fd");while(readdir(d))n++;closedir(d);return n;}
/* fails if the process keeps more descriptors after repeated display cycles */
int main(void){
  int before=0;
  for(int i=0;i<30;i++){
    EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY); eglInitialize(d,0,0);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE}; EGLConfig c; EGLint n;
    eglChooseConfig(d,ca,&c,1,&n);
    EGLint xa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext x=eglCreateContext(d,c,EGL_NO_CONTEXT,xa);
    eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,x);
    glFinish();
    eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(d,x); eglTerminate(d);
    if(i==4) before=nfds();
  }
  int after=nfds();
  printf("fds after warm-up=%d, after 25 more display cycles=%d\n",before,after);
  return after>before?1:0;}
