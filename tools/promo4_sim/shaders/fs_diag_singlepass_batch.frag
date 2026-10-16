uniform sampler2DRect refTex;
uniform sampler2DRect blockInfoTex;
float clip255(float v) { return max(0.0, min(255.0, v)); }
float tap6raw(vec2 b) {
  float a2=texture2DRect(refTex,b+vec2(-2.0,0.0)).r*255.0;
  float a1=texture2DRect(refTex,b+vec2(-1.0,0.0)).r*255.0;
  float a0=texture2DRect(refTex,b+vec2( 0.0,0.0)).r*255.0;
  float a3=texture2DRect(refTex,b+vec2( 1.0,0.0)).r*255.0;
  float a4=texture2DRect(refTex,b+vec2( 2.0,0.0)).r*255.0;
  float a5=texture2DRect(refTex,b+vec2( 3.0,0.0)).r*255.0;
  return (a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);
}
float diagExact(vec2 b) {
  float hm2=tap6raw(b+vec2(0.0,-2.0));
  float hm1=tap6raw(b+vec2(0.0,-1.0));
  float h0 =tap6raw(b+vec2(0.0, 0.0));
  float h1 =tap6raw(b+vec2(0.0, 1.0));
  float h2 =tap6raw(b+vec2(0.0, 2.0));
  float h3 =tap6raw(b+vec2(0.0, 3.0));
  float v = (h0+h1)*20.0 - (hm1+h2)*5.0 + (hm2+h3);
  return clip255(floor((v+512.0)/1024.0));
}
float halfH(vec2 b) {
  float a2=texture2DRect(refTex,b+vec2(-2.0,0.0)).r*255.0;
  float a1=texture2DRect(refTex,b+vec2(-1.0,0.0)).r*255.0;
  float a0=texture2DRect(refTex,b+vec2( 0.0,0.0)).r*255.0;
  float a3=texture2DRect(refTex,b+vec2( 1.0,0.0)).r*255.0;
  float a4=texture2DRect(refTex,b+vec2( 2.0,0.0)).r*255.0;
  float a5=texture2DRect(refTex,b+vec2( 3.0,0.0)).r*255.0;
  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);
  return clip255(floor((v+16.0)/32.0));
}
float halfV(vec2 b) {
  float a2=texture2DRect(refTex,b+vec2(0.0,-2.0)).r*255.0;
  float a1=texture2DRect(refTex,b+vec2(0.0,-1.0)).r*255.0;
  float a0=texture2DRect(refTex,b+vec2(0.0, 0.0)).r*255.0;
  float a3=texture2DRect(refTex,b+vec2(0.0, 1.0)).r*255.0;
  float a4=texture2DRect(refTex,b+vec2(0.0, 2.0)).r*255.0;
  float a5=texture2DRect(refTex,b+vec2(0.0, 3.0)).r*255.0;
  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);
  return clip255(floor((v+16.0)/32.0));
}
void main() {
  float col = floor(gl_FragCoord.x);
  float rowr = floor(gl_FragCoord.y);
  float blockIdx = floor(col / 16.0);
  float localX = col - blockIdx * 16.0;
  vec4 info = texture2DRect(blockInfoTex, vec2(blockIdx + 0.5, 0.5));
  float hPhase = info.b, vPhase = info.a;
  vec2 b = vec2(info.r + localX + 0.5, info.g + rowr + 0.5);
  float diag = diagExact(b);
  float result = diag;
  if (hPhase == 2.0 && vPhase == 2.0) {
    result = diag;
  } else if (hPhase == 2.0) {
    float ro = (vPhase == 3.0) ? 1.0 : 0.0;
    float hOp = halfH(b + vec2(0.0, ro));
    result = floor((hOp + diag + 1.0) / 2.0);
  } else {
    float co = (hPhase == 3.0) ? 1.0 : 0.0;
    float vOp = halfV(b + vec2(co, 0.0));
    result = floor((vOp + diag + 1.0) / 2.0);
  }
  gl_FragColor = vec4(result/255.0, 0.0, 0.0, 1.0);
}
