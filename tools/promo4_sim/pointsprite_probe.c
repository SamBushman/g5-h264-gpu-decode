#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
static GLhandleARB mk(const char*vs,const char*fs){ GLhandleARB p=glCreateProgramObjectARB(),a=glCreateShaderObjectARB(GL_VERTEX_SHADER_ARB),b=glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB); glShaderSourceARB(a,1,&vs,0); glCompileShaderARB(a); glShaderSourceARB(b,1,&fs,0); glCompileShaderARB(b); glAttachObjectARB(p,a); glAttachObjectARB(p,b); glLinkProgramARB(p); return p; }
int main(){ GLint at[]={AGL_RGBA,AGL_RED_SIZE,8,AGL_GREEN_SIZE,8,AGL_BLUE_SIZE,8,AGL_ALPHA_SIZE,8,AGL_NONE}; AGLPixelFormat pf=aglChoosePixelFormat(NULL,0,at); AGLContext c=aglCreateContext(pf,NULL); AGLPbuffer pb; aglCreatePBuffer(64,64,GL_TEXTURE_RECTANGLE_ARB,GL_RGBA,0,&pb); aglSetPBuffer(c,pb,0,0,0); aglSetCurrentContext(c);
 glViewport(0,0,64,64); glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,64,0,64,-1,1); glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glPointSize(8.0f);
 const char*vs="varying vec2 ctr; void main(){ ctr=gl_Vertex.xy; gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex; }";
 const char*vs_tc="varying vec2 ctr; void main(){ ctr=gl_Vertex.xy; gl_TexCoord[0]=gl_MultiTexCoord0; gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex; }";
 const char*fsA="void main(){ vec2 f=floor(gl_FragCoord.xy)-vec2(28.0,28.0); gl_FragColor=vec4(f.x/8.0,f.y/8.0,0.0,1.0);} ";
 const char*fsB="varying vec2 ctr; void main(){ gl_FragColor=vec4(gl_TexCoord[0].x, gl_TexCoord[0].y, 0.0, 1.0);} ";
 const char*fsC="varying vec2 ctr; void main(){ gl_FragColor=vec4(ctr.x/64.0, ctr.y/64.0, 0.0, 1.0);} ";
 const char*names[3]={"gl_FragCoord-28 /8","gl_TexCoord[0].xy (sprite replace)","ctr varying/64"};
 const char*fs[3]={fsA,fsB,fsC}; int k, mode;
 for(mode=0;mode<3;mode++){ if(mode==1){glActiveTexture(GL_TEXTURE0); glTexEnvi(GL_POINT_SPRITE_ARB,GL_COORD_REPLACE_ARB,GL_TRUE); glEnable(GL_POINT_SPRITE_ARB);} else glDisable(GL_POINT_SPRITE_ARB);
  GLhandleARB p=mk(vs,fs[mode]); glUseProgramObjectARB(p); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT); glBegin(GL_POINTS); glVertex2f(32,32); glEnd(); glFinish();
  unsigned char px[64*64*4]; glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,px);
  printf("%s: rows y=35..28, cols x=28..35 (R,G):\n",names[mode]); int y,x; for(y=35;y>=28;y-=7){ for(x=28;x<=35;x+=1) printf("(%3d,%3d) ",px[(y*64+x)*4],px[(y*64+x)*4+1]); printf("\n"); }
 }
 return 0;}
