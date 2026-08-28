/* Item 9 follow-up (2026-08-28): standalone verification for the diagonal-
 * family half of the "merge MC + IDCT" redesign - see mc_idct_merge_verify.c
 * for the singlepass-family version (already verified byte-exact) and its
 * header comment for the overall rationale/method. Diagonal MC needs a
 * genuine 2-pass structure (quirk #14, FP24 precision - stage1's rounding
 * MUST be a real texture round-trip, can't be fused away). This keeps
 * stage1 completely UNCHANGED (same shader, same fixed-16-wide-per-
 * partition-lane layout in s1Tex) and merges stage2 (today: partition-
 * lane-addressed, vw-wide x 16-tall viewport) with IDCT (4x4-BLOCK-
 * addressed, n_blocks*4-wide x 4-tall viewport) into ONE new shader -
 * only stage2's own OUTPUT addressing changes, not stage1's.
 *
 * The real new wrinkle vs. the singlepass merge: since a partition can be
 * up to 16x16 (4x4 = 16 sub-blocks), and stage1's own layout is still
 * partition/lane-based (unchanged), each output BLOCK now needs to know
 * not just its own ref-space (refX,refY) (as in the singlepass merge) but
 * ALSO where within its partition's fixed 16-wide/21-tall s1Tex lane it
 * should sample from - a (laneColBase, laneRowBase) pair, since a block
 * at partition-local position (bx*4, by*4) reads s1Tex at column
 * (lane*16 + bx*4 + lc) row (by*4 + lr + 2) [+2 for stage1's own top
 * padding]. Tested via a mix of partition sizes (16x16, 8x8, 4x4) to
 * exercise this row/col-base tracking, not just column. */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void checkgl(const char *w) { GLenum e = glGetError(); if (e) fprintf(stderr, "GL err %s: 0x%lx\n", w, (unsigned long)e); }
static GLhandleARB compile(GLenum t, const char *s) {
    GLhandleARB h = glCreateShaderObjectARB(t);
    glShaderSourceARB(h, 1, &s, NULL); glCompileShaderARB(h);
    GLint ok = 0; glGetObjectParameterivARB(h, GL_OBJECT_COMPILE_STATUS_ARB, &ok);
    if (!ok) { char log[4096]; GLsizei n; glGetInfoLogARB(h, sizeof log, &n, log); fprintf(stderr, "compile fail:\n%s\n%s\n", log, s); exit(1); }
    return h;
}
static GLhandleARB linkp(const char *vs, const char *fs) {
    GLhandleARB p = glCreateProgramObjectARB();
    glAttachObjectARB(p, compile(GL_VERTEX_SHADER_ARB, vs));
    glAttachObjectARB(p, compile(GL_FRAGMENT_SHADER_ARB, fs));
    glLinkProgramARB(p);
    GLint ok = 0; glGetObjectParameterivARB(p, GL_OBJECT_LINK_STATUS_ARB, &ok);
    if (!ok) { char log[4096]; GLsizei n; glGetInfoLogARB(p, sizeof log, &n, log); fprintf(stderr, "link fail:\n%s\n", log); exit(1); }
    return p;
}
static const char *vs_plain = "void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;}";
static float clip255f(float v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* ---- existing, already-proven shaders (verbatim) ---- */
static const char *fs_diag_stage1_batch =
"uniform sampler2DRect refTex;\n"
"uniform sampler2DRect blockInfoTex;\n"
"void main() {\n"
"  float col = floor(gl_FragCoord.x);\n"
"  float rowr = floor(gl_FragCoord.y);\n"
"  float blockIdx = floor(col / 16.0);\n"
"  float localX = col - blockIdx * 16.0;\n"
"  vec4 info = texture2DRect(blockInfoTex, vec2(blockIdx + 0.5, 0.5));\n"
"  vec2 b = vec2(info.r + localX + 0.5, info.g - 2.0 + rowr + 0.5);\n"
"  float a2=texture2DRect(refTex,b+vec2(-2.0,0.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(-1.0,0.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2( 0.0,0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2( 1.0,0.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2( 2.0,0.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2( 3.0,0.0)).r*255.0;\n"
"  float raw = (a0+a3)*20.0 - (a1+a4)*5.0 + (a2+a5);\n"
"  gl_FragColor = vec4(floor((raw+16.0)/32.0), 0.0, 0.0, 1.0);\n"
"}\n";

static const char *fs_diag_stage2_batch =
"uniform sampler2DRect stage1Tex;\n"
"uniform sampler2DRect refTex;\n"
"uniform sampler2DRect blockInfoTex;\n"
"float clip255(float v) { return max(0.0, min(255.0, v)); }\n"
"float dec(vec2 b, float dy) { return texture2DRect(stage1Tex, b+vec2(0.0,dy)).r; }\n"
"float halfH(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(-2.0,0.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(-1.0,0.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2( 0.0,0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2( 1.0,0.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2( 2.0,0.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2( 3.0,0.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"float halfV(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(0.0,-2.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(0.0,-1.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2(0.0, 0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2(0.0, 1.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2(0.0, 2.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2(0.0, 3.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"void main() {\n"
"  float col = floor(gl_FragCoord.x);\n"
"  float rowr = floor(gl_FragCoord.y);\n"
"  float blockIdx = floor(col / 16.0);\n"
"  float localX = col - blockIdx * 16.0;\n"
"  vec4 info = texture2DRect(blockInfoTex, vec2(blockIdx + 0.5, 0.5));\n"
"  float hPhase = info.b, vPhase = info.a;\n"
"  vec2 b = vec2(blockIdx*16.0 + localX + 0.5, rowr + 2.5);\n"
"  float hm2=dec(b,-2.0); float hm1=dec(b,-1.0); float h0=dec(b,0.0);\n"
"  float h1=dec(b,1.0);   float h2=dec(b,2.0);    float h3=dec(b,3.0);\n"
"  float v = (h0+h1)*20.0 - (hm1+h2)*5.0 + (hm2+h3);\n"
"  float diag = clip255(floor((v+16.0)/32.0));\n"
"  vec2 rb = vec2(info.r + localX + 0.5, info.g + rowr + 0.5);\n"
"  float halfH0 = halfH(rb);\n"
"  float halfH1 = halfH(rb + vec2(0.0,1.0));\n"
"  float halfV0 = halfV(rb);\n"
"  float halfV1 = halfV(rb + vec2(1.0,0.0));\n"
"  float result = diag;\n"
"  if (hPhase == 2.0 && vPhase == 2.0) {\n"
"    result = diag;\n"
"  } else if (hPhase == 2.0) {\n"
"    float hOp = halfH0; if (vPhase == 3.0) hOp = halfH1;\n"
"    result = floor((hOp + diag + 1.0) / 2.0);\n"
"  } else {\n"
"    float vOp = halfV0; if (hPhase == 3.0) vOp = halfV1;\n"
"    result = floor((vOp + diag + 1.0) / 2.0);\n"
"  }\n"
"  gl_FragColor = vec4(result/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

static const char *fs_idct_batch =
"uniform sampler2DRect coeffTex;\n"
"void main() {\n"
"  vec2 p = floor(gl_FragCoord.xy);\n"
"  float base = floor(p.x / 4.0) * 4.0;\n"
"  float lc = p.x - base;\n"
"  float lr = p.y;\n"
"  float c0  = texture2DRect(coeffTex, vec2(base+0.5, 0.5)).r;\n"
"  float c1  = texture2DRect(coeffTex, vec2(base+1.5, 0.5)).r;\n"
"  float c2  = texture2DRect(coeffTex, vec2(base+2.5, 0.5)).r;\n"
"  float c3  = texture2DRect(coeffTex, vec2(base+3.5, 0.5)).r;\n"
"  float c4  = texture2DRect(coeffTex, vec2(base+0.5, 1.5)).r;\n"
"  float c5  = texture2DRect(coeffTex, vec2(base+1.5, 1.5)).r;\n"
"  float c6  = texture2DRect(coeffTex, vec2(base+2.5, 1.5)).r;\n"
"  float c7  = texture2DRect(coeffTex, vec2(base+3.5, 1.5)).r;\n"
"  float c8  = texture2DRect(coeffTex, vec2(base+0.5, 2.5)).r;\n"
"  float c9  = texture2DRect(coeffTex, vec2(base+1.5, 2.5)).r;\n"
"  float c10 = texture2DRect(coeffTex, vec2(base+2.5, 2.5)).r;\n"
"  float c11 = texture2DRect(coeffTex, vec2(base+3.5, 2.5)).r;\n"
"  float c12 = texture2DRect(coeffTex, vec2(base+0.5, 3.5)).r;\n"
"  float c13 = texture2DRect(coeffTex, vec2(base+1.5, 3.5)).r;\n"
"  float c14 = texture2DRect(coeffTex, vec2(base+2.5, 3.5)).r;\n"
"  float c15 = texture2DRect(coeffTex, vec2(base+3.5, 3.5)).r;\n"
"  float z0, z1, z2, z3;\n"
"  float m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15;\n"
"  z0 = c0 + c8;  z1 = c0 - c8;  z2 = floor(c4/2.0) - c12; z3 = c4 + floor(c12/2.0);\n"
"  m0 = z0+z3; m4 = z1+z2; m8 = z1-z2; m12 = z0-z3;\n"
"  z0 = c1 + c9;  z1 = c1 - c9;  z2 = floor(c5/2.0) - c13; z3 = c5 + floor(c13/2.0);\n"
"  m1 = z0+z3; m5 = z1+z2; m9 = z1-z2; m13 = z0-z3;\n"
"  z0 = c2 + c10; z1 = c2 - c10; z2 = floor(c6/2.0) - c14; z3 = c6 + floor(c14/2.0);\n"
"  m2 = z0+z3; m6 = z1+z2; m10 = z1-z2; m14 = z0-z3;\n"
"  z0 = c3 + c11; z1 = c3 - c11; z2 = floor(c7/2.0) - c15; z3 = c7 + floor(c15/2.0);\n"
"  m3 = z0+z3; m7 = z1+z2; m11 = z1-z2; m15 = z0-z3;\n"
"  float o0,o1,o2,o3,o4,o5,o6,o7,o8,o9,o10,o11,o12,o13,o14,o15;\n"
"  z0=m0+m2;  z1=m0-m2;  z2=floor(m1/2.0)-m3;   z3=m1+floor(m3/2.0);\n"
"  o0=floor((z0+z3)/64.0); o4=floor((z1+z2)/64.0); o8=floor((z1-z2)/64.0); o12=floor((z0-z3)/64.0);\n"
"  z0=m4+m6;  z1=m4-m6;  z2=floor(m5/2.0)-m7;   z3=m5+floor(m7/2.0);\n"
"  o1=floor((z0+z3)/64.0); o5=floor((z1+z2)/64.0); o9=floor((z1-z2)/64.0); o13=floor((z0-z3)/64.0);\n"
"  z0=m8+m10; z1=m8-m10; z2=floor(m9/2.0)-m11;  z3=m9+floor(m11/2.0);\n"
"  o2=floor((z0+z3)/64.0); o6=floor((z1+z2)/64.0); o10=floor((z1-z2)/64.0); o14=floor((z0-z3)/64.0);\n"
"  z0=m12+m14;z1=m12-m14;z2=floor(m13/2.0)-m15; z3=m13+floor(m15/2.0);\n"
"  o3=floor((z0+z3)/64.0); o7=floor((z1+z2)/64.0); o11=floor((z1-z2)/64.0); o15=floor((z0-z3)/64.0);\n"
"  int idx = int(lc) + int(lr) * 4;\n"
"  float result = o0;\n"
"  if (idx == 1) result = o1; else if (idx == 2) result = o2; else if (idx == 3) result = o3;\n"
"  else if (idx == 4) result = o4; else if (idx == 5) result = o5; else if (idx == 6) result = o6; else if (idx == 7) result = o7;\n"
"  else if (idx == 8) result = o8; else if (idx == 9) result = o9; else if (idx == 10) result = o10; else if (idx == 11) result = o11;\n"
"  else if (idx == 12) result = o12; else if (idx == 13) result = o13; else if (idx == 14) result = o14; else if (idx == 15) result = o15;\n"
"  float biased = result + 32768.0;\n"
"  float hi = floor(biased / 256.0);\n"
"  float lo = biased - hi * 256.0;\n"
"  gl_FragColor = vec4(hi/255.0, lo/255.0, 0.0, 1.0);\n"
"}\n";

/* ---- NEW: merged stage2+IDCT shader ---- */
static const char *fs_diag_stage2_idct_merged =
"uniform sampler2DRect stage1Tex;\n"
"uniform sampler2DRect refTex;\n"
"uniform sampler2DRect blockInfoTex;\n"  /* per-BLOCK: (refX, refY, hPhase, vPhase) */
"uniform sampler2DRect laneInfoTex;\n"   /* per-BLOCK: (s1ColBase, s1RowBase) */
"uniform sampler2DRect coeffTex;\n"
"float clip255(float v) { return max(0.0, min(255.0, v)); }\n"
"float dec(vec2 b, float dy) { return texture2DRect(stage1Tex, b+vec2(0.0,dy)).r; }\n"
"float halfH(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(-2.0,0.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(-1.0,0.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2( 0.0,0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2( 1.0,0.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2( 2.0,0.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2( 3.0,0.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"float halfV(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(0.0,-2.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(0.0,-1.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2(0.0, 0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2(0.0, 1.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2(0.0, 2.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2(0.0, 3.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"void main() {\n"
"  float p_x = floor(gl_FragCoord.x);\n"
"  float p_y = floor(gl_FragCoord.y);\n"
"  float blockIdx = floor(p_x / 4.0);\n"
"  float lc = p_x - blockIdx * 4.0;\n"
"  float lr = p_y;\n"
"  vec4 info = texture2DRect(blockInfoTex, vec2(blockIdx + 0.5, 0.5));\n"
"  float hPhase = info.b, vPhase = info.a;\n"
"  vec4 laneInfo = texture2DRect(laneInfoTex, vec2(blockIdx + 0.5, 0.5));\n"
"  vec2 b = vec2(laneInfo.r + lc + 0.5, laneInfo.g + lr + 2.5);\n"
"  float hm2=dec(b,-2.0); float hm1=dec(b,-1.0); float h0=dec(b,0.0);\n"
"  float h1=dec(b,1.0);   float h2=dec(b,2.0);    float h3=dec(b,3.0);\n"
"  float v = (h0+h1)*20.0 - (hm1+h2)*5.0 + (hm2+h3);\n"
"  float diag = clip255(floor((v+16.0)/32.0));\n"
"  vec2 rb = vec2(info.r + lc, info.g + lr);\n"
"  float halfH0 = halfH(rb);\n"
"  float halfH1 = halfH(rb + vec2(0.0,1.0));\n"
"  float halfV0 = halfV(rb);\n"
"  float halfV1 = halfV(rb + vec2(1.0,0.0));\n"
"  float pred = diag;\n"
"  if (hPhase == 2.0 && vPhase == 2.0) {\n"
"    pred = diag;\n"
"  } else if (hPhase == 2.0) {\n"
"    float hOp = halfH0; if (vPhase == 3.0) hOp = halfH1;\n"
"    pred = floor((hOp + diag + 1.0) / 2.0);\n"
"  } else {\n"
"    float vOp = halfV0; if (hPhase == 3.0) vOp = halfV1;\n"
"    pred = floor((vOp + diag + 1.0) / 2.0);\n"
"  }\n"
"  float base = blockIdx * 4.0;\n"
"  float c0  = texture2DRect(coeffTex, vec2(base+0.5, 0.5)).r;\n"
"  float c1  = texture2DRect(coeffTex, vec2(base+1.5, 0.5)).r;\n"
"  float c2  = texture2DRect(coeffTex, vec2(base+2.5, 0.5)).r;\n"
"  float c3  = texture2DRect(coeffTex, vec2(base+3.5, 0.5)).r;\n"
"  float c4  = texture2DRect(coeffTex, vec2(base+0.5, 1.5)).r;\n"
"  float c5  = texture2DRect(coeffTex, vec2(base+1.5, 1.5)).r;\n"
"  float c6  = texture2DRect(coeffTex, vec2(base+2.5, 1.5)).r;\n"
"  float c7  = texture2DRect(coeffTex, vec2(base+3.5, 1.5)).r;\n"
"  float c8  = texture2DRect(coeffTex, vec2(base+0.5, 2.5)).r;\n"
"  float c9  = texture2DRect(coeffTex, vec2(base+1.5, 2.5)).r;\n"
"  float c10 = texture2DRect(coeffTex, vec2(base+2.5, 2.5)).r;\n"
"  float c11 = texture2DRect(coeffTex, vec2(base+3.5, 2.5)).r;\n"
"  float c12 = texture2DRect(coeffTex, vec2(base+0.5, 3.5)).r;\n"
"  float c13 = texture2DRect(coeffTex, vec2(base+1.5, 3.5)).r;\n"
"  float c14 = texture2DRect(coeffTex, vec2(base+2.5, 3.5)).r;\n"
"  float c15 = texture2DRect(coeffTex, vec2(base+3.5, 3.5)).r;\n"
"  float z0, z1, z2, z3;\n"
"  float m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15;\n"
"  z0 = c0 + c8;  z1 = c0 - c8;  z2 = floor(c4/2.0) - c12; z3 = c4 + floor(c12/2.0);\n"
"  m0 = z0+z3; m4 = z1+z2; m8 = z1-z2; m12 = z0-z3;\n"
"  z0 = c1 + c9;  z1 = c1 - c9;  z2 = floor(c5/2.0) - c13; z3 = c5 + floor(c13/2.0);\n"
"  m1 = z0+z3; m5 = z1+z2; m9 = z1-z2; m13 = z0-z3;\n"
"  z0 = c2 + c10; z1 = c2 - c10; z2 = floor(c6/2.0) - c14; z3 = c6 + floor(c14/2.0);\n"
"  m2 = z0+z3; m6 = z1+z2; m10 = z1-z2; m14 = z0-z3;\n"
"  z0 = c3 + c11; z1 = c3 - c11; z2 = floor(c7/2.0) - c15; z3 = c7 + floor(c15/2.0);\n"
"  m3 = z0+z3; m7 = z1+z2; m11 = z1-z2; m15 = z0-z3;\n"
"  float o0,o1,o2,o3,o4,o5,o6,o7,o8,o9,o10,o11,o12,o13,o14,o15;\n"
"  z0=m0+m2;  z1=m0-m2;  z2=floor(m1/2.0)-m3;   z3=m1+floor(m3/2.0);\n"
"  o0=floor((z0+z3)/64.0); o4=floor((z1+z2)/64.0); o8=floor((z1-z2)/64.0); o12=floor((z0-z3)/64.0);\n"
"  z0=m4+m6;  z1=m4-m6;  z2=floor(m5/2.0)-m7;   z3=m5+floor(m7/2.0);\n"
"  o1=floor((z0+z3)/64.0); o5=floor((z1+z2)/64.0); o9=floor((z1-z2)/64.0); o13=floor((z0-z3)/64.0);\n"
"  z0=m8+m10; z1=m8-m10; z2=floor(m9/2.0)-m11;  z3=m9+floor(m11/2.0);\n"
"  o2=floor((z0+z3)/64.0); o6=floor((z1+z2)/64.0); o10=floor((z1-z2)/64.0); o14=floor((z0-z3)/64.0);\n"
"  z0=m12+m14;z1=m12-m14;z2=floor(m13/2.0)-m15; z3=m13+floor(m15/2.0);\n"
"  o3=floor((z0+z3)/64.0); o7=floor((z1+z2)/64.0); o11=floor((z1-z2)/64.0); o15=floor((z0-z3)/64.0);\n"
"  int idx = int(lc) + int(lr) * 4;\n"
"  float residual = o0;\n"
"  if (idx == 1) residual = o1; else if (idx == 2) residual = o2; else if (idx == 3) residual = o3;\n"
"  else if (idx == 4) residual = o4; else if (idx == 5) residual = o5; else if (idx == 6) residual = o6; else if (idx == 7) residual = o7;\n"
"  else if (idx == 8) residual = o8; else if (idx == 9) residual = o9; else if (idx == 10) residual = o10; else if (idx == 11) residual = o11;\n"
"  else if (idx == 12) residual = o12; else if (idx == 13) residual = o13; else if (idx == 14) residual = o14; else if (idx == 15) residual = o15;\n"
"  float finalPix = clip255(pred + residual);\n"
"  gl_FragColor = vec4(finalPix/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

#define REFW 96
#define REFH 96

/* Three test partitions of different sizes: a 16x16 (P0), an 8x8 (P1), a
 * 4x4 (P2) - exercises row-base AND col-base tracking (P0 alone spans 16
 * output blocks at row-bases 0/4/8/12 and col-bases 0/4/8/12). Every
 * partition uses hPhase=2,vPhase=2 (pure diagonal) for one sub-block and
 * mixed phases for others, to exercise every branch in the blend logic. */
typedef struct { int px, py, w, h, hphase, vphase; } TestPart;
static TestPart parts[3] = {
    {20, 20, 16, 16, 2, 2},
    {40, 30, 8, 8, 1, 2},
    {50, 45, 4, 4, 2, 3},
};
#define NPARTS 3

int main(void) {
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8,
                        AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(1024, 32, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);

    static unsigned char refimg[REFH][REFW];
    for (int y = 0; y < REFH; y++)
        for (int x = 0; x < REFW; x++)
            refimg[y][x] = (unsigned char)((x * 7 + y * 13 + (x*y) % 23) % 256);
    GLuint refTex;
    glGenTextures(1, &refTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_LUMINANCE8, REFW, REFH, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, refimg);

    /* Total 4x4 blocks across all partitions. */
    int nblocks = 0;
    for (int i = 0; i < NPARTS; i++) nblocks += (parts[i].w/4) * (parts[i].h/4);

    /* Old-path stage1/stage2 blockInfoTex: per PARTITION (lane), (pel_x,
     * pel_y, hphase, vphase) - matches the real code exactly. */
    GLuint partInfoTex;
    { static float pi[NPARTS][4];
      for (int i = 0; i < NPARTS; i++) {
          pi[i][0]=(float)parts[i].px; pi[i][1]=(float)parts[i].py;
          pi[i][2]=(float)parts[i].hphase; pi[i][3]=(float)parts[i].vphase;
      }
      glGenTextures(1, &partInfoTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, partInfoTex);
      glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, NPARTS, 1, 0, GL_RGBA, GL_FLOAT, pi);
    }

    /* Per-BLOCK info for the NEW merged shader: blockInfoTex (refX,refY,
     * hphase,vphase - block's own top-left) and laneInfoTex (s1ColBase,
     * s1RowBase). Also record, per block, which OLD-path (lane,localX,
     * localY) it corresponds to, to build the old-path comparison. */
    static float blockInfo[64*4], laneInfo[64*4];
    static int oldLane[64], oldLocalX[64], oldLocalY[64];
    { int bi = 0;
      for (int i = 0; i < NPARTS; i++) {
          int bw = parts[i].w/4, bh = parts[i].h/4;
          for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
              blockInfo[bi*4+0] = (float)(parts[i].px + bx*4);
              blockInfo[bi*4+1] = (float)(parts[i].py + by*4);
              blockInfo[bi*4+2] = (float)parts[i].hphase;
              blockInfo[bi*4+3] = (float)parts[i].vphase;
              laneInfo[bi*4+0] = (float)(i*16 + bx*4);
              laneInfo[bi*4+1] = (float)(by*4);
              laneInfo[bi*4+2] = 0; laneInfo[bi*4+3] = 1;
              oldLane[bi] = i; oldLocalX[bi] = bx*4; oldLocalY[bi] = by*4;
              bi++;
          }
      }
    }
    GLuint blockInfoTexNew, laneInfoTex;
    glGenTextures(1, &blockInfoTexNew); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTexNew);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, nblocks, 1, 0, GL_RGBA, GL_FLOAT, blockInfo);
    glGenTextures(1, &laneInfoTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, laneInfoTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, nblocks, 1, 0, GL_RGBA, GL_FLOAT, laneInfo);

    /* Coefficients, one 4x4 set per block. */
    static int coeffs[64][16];
    for (int b = 0; b < nblocks; b++)
        for (int i = 0; i < 16; i++)
            coeffs[b][i] = ((b * 31 + i * 17) % 61) - 30 + (b == 2 ? 400 : 0) - (b == 5 ? 400 : 0);
    GLuint coeffTex;
    { static float ctd[4 * 64*4 * 4];
      for (int b = 0; b < nblocks; b++)
          for (int row = 0; row < 4; row++)
              for (int col = 0; col < 4; col++) {
                  int texel = row * (nblocks*4) + (b*4+col);
                  ctd[texel*4+0] = (float)coeffs[b][row*4+col];
                  ctd[texel*4+1] = ctd[texel*4+2] = 0.0f; ctd[texel*4+3] = 1.0f;
              }
      glGenTextures(1, &coeffTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, coeffTex);
      glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, nblocks*4, 4, 0, GL_RGBA, GL_FLOAT, ctd);
    }
    checkgl("setup");

    /* ---- stage1 (shared, unchanged) - render into a real FBO texture ---- */
    GLuint s1Tex, fbo;
    glGenTextures(1, &s1Tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, s1Tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, NPARTS*16, 21, 0, GL_RGBA, GL_FLOAT, NULL);
    glGenFramebuffersEXT(1, &fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_RECTANGLE_ARB, s1Tex, 0);
    if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) { fprintf(stderr, "s1 FBO incomplete\n"); return 1; }

    GLhandleARB progA = linkp(vs_plain, fs_diag_stage1_batch);
    glUseProgramObjectARB(progA);
    glViewport(0, 0, NPARTS*16, 21);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, NPARTS*16, 0, 21, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex);
    glUniform1iARB(glGetUniformLocationARB(progA, "refTex"), 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, partInfoTex);
    glUniform1iARB(glGetUniformLocationARB(progA, "blockInfoTex"), 1);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0); glVertex2f(NPARTS*16,0); glVertex2f(NPARTS*16,21); glVertex2f(0,21); glEnd();
    checkgl("stage1 draw");
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

    /* ---- Old path: stage2 (16-wide-lane addressed, back to default buffer) ---- */
    static unsigned char predpix[NPARTS*16][16];
    { GLhandleARB progB = linkp(vs_plain, fs_diag_stage2_batch);
      glUseProgramObjectARB(progB);
      glViewport(0, 0, NPARTS*16, 16);
      glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, NPARTS*16, 0, 16, -1, 1);
      glMatrixMode(GL_MODELVIEW); glLoadIdentity();
      glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, s1Tex);
      glUniform1iARB(glGetUniformLocationARB(progB, "stage1Tex"), 0);
      glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex);
      glUniform1iARB(glGetUniformLocationARB(progB, "refTex"), 1);
      glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, partInfoTex);
      glUniform1iARB(glGetUniformLocationARB(progB, "blockInfoTex"), 2);
      glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
      glBegin(GL_QUADS); glVertex2f(0,0); glVertex2f(NPARTS*16,0); glVertex2f(NPARTS*16,16); glVertex2f(0,16); glEnd();
      checkgl("old stage2 draw");
      static unsigned char rgba[NPARTS*16*16*4];
      glReadPixels(0,0,NPARTS*16,16,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
      for (int y = 0; y < 16; y++) for (int x = 0; x < NPARTS*16; x++) predpix[x][y] = rgba[(y*NPARTS*16+x)*4];
    }

    /* ---- Old path: IDCT (block-addressed, using the SAME coeffTex) ---- */
    static int residual[64][4][4];
    { GLhandleARB progI = linkp(vs_plain, fs_idct_batch);
      glUseProgramObjectARB(progI);
      glViewport(0, 0, nblocks*4, 4);
      glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, nblocks*4, 0, 4, -1, 1);
      glMatrixMode(GL_MODELVIEW); glLoadIdentity();
      glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, coeffTex);
      glUniform1iARB(glGetUniformLocationARB(progI, "coeffTex"), 0);
      glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
      glBegin(GL_QUADS); glVertex2f(0,0); glVertex2f(nblocks*4,0); glVertex2f(nblocks*4,4); glVertex2f(0,4); glEnd();
      checkgl("old idct draw");
      static unsigned char rgba[64*4*4*4];
      glReadPixels(0,0,nblocks*4,4,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
      for (int y = 0; y < 4; y++) for (int x = 0; x < nblocks*4; x++) {
          int b = x/4, lc = x%4;
          int hi = rgba[(y*nblocks*4+x)*4+0], lo = rgba[(y*nblocks*4+x)*4+1];
          residual[b][y][lc] = (hi*256+lo) - 32768;
      }
    }

    /* ---- Old path: CPU combine, mapped through oldLane/oldLocalX/Y ---- */
    static unsigned char oldFinal[64][4][4];
    for (int b = 0; b < nblocks; b++)
        for (int lr = 0; lr < 4; lr++) for (int lc = 0; lc < 4; lc++) {
            int px = oldLane[b]*16 + oldLocalX[b] + lc;
            int py = oldLocalY[b] + lr;
            oldFinal[b][lr][lc] = (unsigned char)clip255f((float)predpix[px][py] + (float)residual[b][lr][lc]);
        }

    /* ---- New path: merged stage2+IDCT ---- */
    static unsigned char newFinal[64][4][4];
    { GLhandleARB progM = linkp(vs_plain, fs_diag_stage2_idct_merged);
      glUseProgramObjectARB(progM);
      glViewport(0, 0, nblocks*4, 4);
      glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, nblocks*4, 0, 4, -1, 1);
      glMatrixMode(GL_MODELVIEW); glLoadIdentity();
      glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, s1Tex);
      glUniform1iARB(glGetUniformLocationARB(progM, "stage1Tex"), 0);
      glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex);
      glUniform1iARB(glGetUniformLocationARB(progM, "refTex"), 1);
      glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTexNew);
      glUniform1iARB(glGetUniformLocationARB(progM, "blockInfoTex"), 2);
      glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, laneInfoTex);
      glUniform1iARB(glGetUniformLocationARB(progM, "laneInfoTex"), 3);
      glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, coeffTex);
      glUniform1iARB(glGetUniformLocationARB(progM, "coeffTex"), 4);
      glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
      glBegin(GL_QUADS); glVertex2f(0,0); glVertex2f(nblocks*4,0); glVertex2f(nblocks*4,4); glVertex2f(0,4); glEnd();
      checkgl("merged draw");
      static unsigned char rgba[64*4*4*4];
      glReadPixels(0,0,nblocks*4,4,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
      for (int y = 0; y < 4; y++) for (int x = 0; x < nblocks*4; x++) {
          int b = x/4, lc = x%4;
          newFinal[b][y][lc] = rgba[(y*nblocks*4+x)*4];
      }
    }

    /* ---- Compare ---- */
    int mism = 0, worst = 0;
    for (int b = 0; b < nblocks; b++) {
        int blockmism = 0, blockworst = 0;
        for (int lr = 0; lr < 4; lr++) for (int lc = 0; lc < 4; lc++) {
            int d = abs((int)oldFinal[b][lr][lc] - (int)newFinal[b][lr][lc]);
            if (d > 0) { blockmism++; mism++; }
            if (d > blockworst) blockworst = d;
            if (d > worst) worst = d;
        }
        printf("block %2d (lane=%d localX=%d localY=%d): %d/16 mismatch, worst diff %d\n",
               b, oldLane[b], oldLocalX[b], oldLocalY[b], blockmism, blockworst);
    }
    printf("\nTOTAL: %d/%d pixels differ, worst diff %d\n", mism, nblocks*16, worst);
    printf(mism == 0 ? "RESULT: MERGED DIAG SHADER MATCHES OLD TWO-PASS+IDCT EXACTLY\n" : "RESULT: MISMATCH FOUND\n");

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return mism != 0;
}
