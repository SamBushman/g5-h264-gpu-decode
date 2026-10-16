uniform sampler2DRect refTex;
uniform sampler2DRect blockInfoTex;
uniform sampler2DRect colInfoTex;
float clip255(float v) { return max(0.0, min(255.0, v)); }
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
void main(){
  float col = floor(gl_FragCoord.x);
  float row = floor(gl_FragCoord.y);
  vec4 colInfo = texture2DRect(colInfoTex, vec2(col+0.5, 0.5));
  float blockIdx = colInfo.r;
  float localX = colInfo.g;
  vec4 info = texture2DRect(blockInfoTex, vec2(blockIdx+0.5, 0.5));
  vec2 b = vec2(info.r + localX, info.g + row);
  float hPhase = info.b, vPhase = info.a;
  float full00 = texture2DRect(refTex, b).r*255.0;
  float full10 = texture2DRect(refTex, b+vec2(1.0,0.0)).r*255.0;
  float full01 = texture2DRect(refTex, b+vec2(0.0,1.0)).r*255.0;
  float halfH0 = halfH(b);
  float halfH1 = halfH(b+vec2(0.0,1.0));
  float halfV0 = halfV(b);
  float halfV1 = halfV(b+vec2(1.0,0.0));
  float result = full00;
  if (hPhase == 0.0 && vPhase == 0.0) {
    result = full00;
  } else if (hPhase == 2.0 && vPhase == 0.0) {
    result = halfH0;
  } else if (hPhase == 0.0 && vPhase == 2.0) {
    result = halfV0;
  } else if (vPhase == 0.0) {
    float fullOp = full00; if (hPhase == 3.0) fullOp = full10;
    result = floor((fullOp + halfH0 + 1.0) / 2.0);
  } else if (hPhase == 0.0) {
    float fullOp = full00; if (vPhase == 3.0) fullOp = full01;
    result = floor((fullOp + halfV0 + 1.0) / 2.0);
  } else {
    float hOp = halfH0; if (vPhase == 3.0) hOp = halfH1;
    float vOp = halfV0; if (hPhase == 3.0) vOp = halfV1;
    result = floor((hOp + vOp + 1.0) / 2.0);
  }
  gl_FragColor = vec4(result/255.0, 0.0, 0.0, 1.0);
}
