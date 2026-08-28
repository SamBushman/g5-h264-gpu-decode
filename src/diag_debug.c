#include "mp4box.h"
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/x1900_hook.h>

static int g_frame_idx=0;
static unsigned char *g_ref_y=NULL; static int g_ref_w,g_ref_h,g_ref_stride;
static int g_have_d=0,g_d_mbx,g_d_mby,g_d_mvx,g_d_mvy;

static int margin_ok(int mb_x,int mb_y,int mvx,int mvy){
    int sx=mb_x*16+(mvx>>2), sy=mb_y*16+(mvy>>2);
    return sx-2>=0 && sx+6<g_ref_w && sy-2>=0 && sy+6<g_ref_h;
}
static int hook(int mb_x,int mb_y,int mb_type,int qscale,const int16_t*coeffs,const uint8_t*nnz,const int16_t*mv_l0,const int8_t*ref_l0,void*ud){
    (void)mb_type;(void)qscale;(void)coeffs;(void)nnz;(void)ud;
    if(g_frame_idx==1 && ref_l0[0]==0 && !g_have_d){
        int fx=mv_l0[0]&3, fy=mv_l0[1]&3;
        if(fx==2 && fy==2 && margin_ok(mb_x,mb_y,mv_l0[0],mv_l0[1])){
            g_d_mbx=mb_x; g_d_mby=mb_y; g_d_mvx=mv_l0[0]; g_d_mvy=mv_l0[1]; g_have_d=1;
        }
    }
    return 0;
}
static int h_raw(const unsigned char*src,int stride,int x,int y){
    const unsigned char*p=src+y*stride+x;
    return (p[0]+p[1])*20-(p[-1]+p[2])*5+(p[-2]+p[3]);
}
static void checkgl(const char*w){GLenum e=glGetError(); if(e) fprintf(stderr,"GL err %s: 0x%lx\n",w,(unsigned long)e);}
static GLhandleARB compile(GLenum t,const char*s){GLhandleARB h=glCreateShaderObjectARB(t);glShaderSourceARB(h,1,&s,NULL);glCompileShaderARB(h);GLint ok=0;glGetObjectParameterivARB(h,GL_OBJECT_COMPILE_STATUS_ARB,&ok);if(!ok){char log[4096];GLsizei n;glGetInfoLogARB(h,sizeof log,&n,log);fprintf(stderr,"compile fail:\n%s\n",log);exit(1);} return h;}
static GLhandleARB linkp(const char*vs,const char*fs){GLhandleARB p=glCreateProgramObjectARB();glAttachObjectARB(p,compile(GL_VERTEX_SHADER_ARB,vs));glAttachObjectARB(p,compile(GL_FRAGMENT_SHADER_ARB,fs));glLinkProgramARB(p);GLint ok=0;glGetObjectParameterivARB(p,GL_OBJECT_LINK_STATUS_ARB,&ok);if(!ok){char log[4096];GLsizei n;glGetInfoLogARB(p,sizeof log,&n,log);fprintf(stderr,"link fail:\n%s\n",log);exit(1);} return p;}

static const char*vs="void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex; gl_TexCoord[0]=gl_MultiTexCoord0;}";
/* output raw h0 (stage-1 only) encoded as result/16384+0.5 (h_raw can be large) */
static const char*fs_h0=
"uniform sampler2DRect refTex;\n"
"void main(){\n"
"  vec2 b = gl_TexCoord[0].xy;\n"
"  float a2=texture2DRect(refTex,b+vec2(-2.0,0.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(-1.0,0.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2( 0.0,0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2( 1.0,0.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2( 2.0,0.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2( 3.0,0.0)).r*255.0;\n"
"  float h = (a0+a3)*20.0 - (a1+a4)*5.0 + (a2+a5);\n"
"  gl_FragColor = vec4(h/16384.0+0.5, 0.0,0.0,1.0);\n"
"}\n";

int main(int argc,char**argv){
    Mp4Movie mov; mp4_open(argv[1],&mov); g_ref_w=mov.width; g_ref_h=mov.height;
    int alen=0; unsigned char*avcc=mp4_build_avcc(&mov,&alen);
    const AVCodec*codec=avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext*ctx=avcodec_alloc_context3(codec);
    ctx->extradata=(uint8_t*)av_mallocz(alen+AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata,avcc,(size_t)alen); ctx->extradata_size=alen;
    avcodec_open2(ctx,codec,NULL);
    ff_x1900_set_mb_hook(hook,NULL);
    AVPacket*pkt=av_packet_alloc(); AVFrame*frame=av_frame_alloc();
    for(uint32_t i=0;i<mov.sample_count && !(g_frame_idx>1||(g_frame_idx==1&&g_have_d));i++){
        Mp4Sample*s=&mov.samples[i];
        av_new_packet(pkt,(int)s->size);
        memcpy(pkt->data,mov.file_data+s->offset,s->size);
        avcodec_send_packet(ctx,pkt); av_packet_unref(pkt);
        while(avcodec_receive_frame(ctx,frame)==0){
            if(g_frame_idx==0){
                g_ref_stride=frame->linesize[0];
                g_ref_y=(unsigned char*)malloc((size_t)g_ref_stride*frame->height);
                memcpy(g_ref_y,frame->data[0],(size_t)g_ref_stride*frame->height);
            }
            g_frame_idx++; av_frame_unref(frame);
        }
    }
    if(!g_have_d){fprintf(stderr,"no candidate\n");return 1;}
    int src_x=g_d_mbx*16+(g_d_mvx>>2), src_y=g_d_mby*16+(g_d_mvy>>2);
    printf("MB(%d,%d) mv=(%d,%d) src=(%d,%d)\n",g_d_mbx,g_d_mby,g_d_mvx,g_d_mvy,src_x,src_y);

    /* CPU h_raw for output pixel (0,0) i.e. position (src_x,src_y) */
    int cpu_h0 = h_raw(g_ref_y, g_ref_stride, src_x, src_y);
    printf("CPU h_raw at (%d,%d) = %d\n", src_x, src_y, cpu_h0);

    GLint attribs[]={AGL_RGBA,AGL_DEPTH_SIZE,24,AGL_NONE};
    AGLPixelFormat pf=aglChoosePixelFormat(NULL,0,attribs);
    AGLContext ctx2=aglCreateContext(pf,NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(4,4,GL_TEXTURE_RECTANGLE_ARB,GL_RGBA,0,&pbuf);
    aglSetPBuffer(ctx2,pbuf,0,0,0); aglSetCurrentContext(ctx2);
    glViewport(0,0,4,4); glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(0,4,0,4,-1,1);
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();

    int pad=3, pw=4+2*pad, ph=4+2*pad;
    float*patch=(float*)malloc(sizeof(float)*pw*ph*4);
    for(int y=0;y<ph;y++)for(int x=0;x<pw;x++){
        unsigned char v=g_ref_y[(src_y-pad+y)*g_ref_stride+(src_x-pad+x)];
        int idx=(y*pw+x)*4; patch[idx]=v/255.0f; patch[idx+1]=patch[idx+2]=0; patch[idx+3]=1;
    }
    GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,pw,ph,0,GL_RGBA,GL_FLOAT,patch);
    checkgl("upload");

    GLhandleARB prog=linkp(vs,fs_h0);
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    glUniform1iARB(glGetUniformLocationARB(prog,"refTex"),0);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glTexCoord2f(pad+0.5f,pad+0.5f); glVertex2f(0,0);
    glTexCoord2f(pad+4.5f,pad+0.5f); glVertex2f(4,0);
    glTexCoord2f(pad+4.5f,pad+4.5f); glVertex2f(4,4);
    glTexCoord2f(pad+0.5f,pad+4.5f); glVertex2f(0,4);
    glEnd(); glFinish(); checkgl("draw");

    unsigned char px[4*4*4];
    glReadPixels(0,0,4,4,GL_RGBA,GL_UNSIGNED_BYTE,px);
    /* bottom-left texel = output (0,0) per the established row convention */
    unsigned char r8 = px[0];
    float decoded = ((r8/255.0f)-0.5f)*16384.0f;
    printf("GPU h_raw (decoded from 8-bit readback) at (0,0) ~= %.1f (quantization step ~64)\n", decoded);
    printf("diff = %.1f\n", decoded - cpu_h0);
    return 0;
}
