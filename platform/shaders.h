#pragma once

namespace sacred {
inline constexpr const char* vertex_shader = R"GLSL(#version 330 core
layout(location=0) in vec4 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec4 diffuse;
layout(location=3) in vec4 specular;
layout(location=4) in vec2 uv0;
layout(location=5) in vec2 uv1;
uniform mat4 world, view, projection, texMatrix0, texMatrix1;
uniform ivec2 texCount;
uniform vec4 viewport;
uniform float pixelBias;
uniform int transformed, lighting, colorVertex, hasDiffuse, hasSpecular;
uniform int meshOn;
uniform vec4 meshTransform;
uniform ivec2 groundLit;
uniform vec4 groundView;
uniform vec2 groundOffset;
uniform ivec2 groundCorners[4];
uniform sampler2D groundLight;
uniform int skinOn, skinNormalize, skinColorOn;
uniform vec4 skinColor;
uniform vec4 bones[192];
uniform int normalizeNormals, localViewer, specularEnable;
uniform ivec4 materialSources;
uniform vec4 material[4];
uniform float power;
uniform vec4 globalAmbient;
uniform int lightType[8];
uniform vec4 lightDiffuse[8], lightAmbient[8], lightSpecular[8];
uniform vec3 lightPosition[8], lightDirection[8], lightAttenuation[8];
uniform vec4 lightSpot[8];
out vec4 vertexDiffuse, vertexSpecular;
out vec2 texcoord[2];
out float eyeDepth;
vec4 vertexColor() { return skinOn != 0 && skinColorOn != 0 ? skinColor : diffuse; }
vec4 materialColor(int source, vec4 fallback) {
    if (colorVertex != 0 && source == 1 && hasDiffuse != 0) return vertexColor();
    if (colorVertex != 0 && source == 2 && hasSpecular != 0) return specular;
    return fallback;
}
// A ground vertex's colour among the world's light sources (GroundLight in renderer.h; the game's 0x41b510 with its
// one-step inverse square root): row 0 of groundLight is the colour ramp, the rows below hold the lights.
vec4 groundColor(vec2 screen) {
    vec2 low=viewport.xy-vec2(128.0), high=viewport.xy+viewport.zw+vec2(128.0);
    if (screen.x<low.x || screen.y<low.y || screen.x>high.x || screen.y>high.y) return diffuse;
    vec2 tile=normal.xy+groundOffset;
    ivec2 at=ivec2((tile*groundView.x-groundView.zw)*groundView.y)+ivec2(groundView.zw)+groundCorners[int(normal.z+0.5)];
    int sum=32;
    for (int i=0;i<groundLit.y && sum<255;++i) {
        ivec3 light=ivec3(texelFetch(groundLight,ivec2(i&255,1+(i>>8)),0).xyz);
        ivec2 d=at-light.xy;
        if (abs(d.y)>light.z) continue;
        float square=float(d.x*d.x+d.y*d.y);
        float root=intBitsToFloat(0x5f3759df-(floatBitsToInt(square)>>1));
        int reach=light.z-int(1.0/(root*(1.5-0.5*square*root*root)));
        sum+=reach>=224 ? 223 : max(reach,0);
    }
    vec3 ramp=texelFetch(groundLight,ivec2(min(sum,255),0),0).xyz;
    return vec4(floor(floor(diffuse.r*255.0+0.5)*ramp/256.0)/255.0,1.0);
}
void main() {
    texcoord[0]=texCount.x==2 ? (texMatrix0*vec4(uv0,1.0,0.0)).xy : uv0;
    texcoord[1]=texCount.y==2 ? (texMatrix1*vec4(uv1,1.0,0.0)).xy : uv1;
    if (transformed != 0) {
        vec4 p=meshOn != 0 ? vec4(position.xy*meshTransform.x+meshTransform.yz,meshTransform.w,1.0) : position;
        float w=1.0/p.w;
        gl_Position=vec4((2.0*(p.xy+vec2(pixelBias)-viewport.xy)/viewport.zw-1.0)*w,
                         (2.0*p.z-1.0)*w,w);
        vertexDiffuse=meshOn != 0 && groundLit.x != 0 ? groundColor(p.xy) : diffuse;
        vertexSpecular=specular; eyeDepth=p.z;
        return;
    }
    vec3 objectPosition=position.xyz, objectNormal=normal;
    if (skinOn != 0) {
        // Granny's deform (skin_positions / skin_normals): the weighted sum of the bones' transforms.
        objectPosition=vec3(0.0); objectNormal=vec3(0.0);
        for (int k=0;k<4;++k) {
            float w=specular[k];
            if (w==0.0) continue;
            int b=3*((int(uv1.x+0.5)>>(6*k))&63);
            vec4 r0=bones[b], r1=bones[b+1], r2=bones[b+2];
            objectPosition+=w*vec3(dot(r0.xyz,position.xyz)+r0.w,dot(r1.xyz,position.xyz)+r1.w,dot(r2.xyz,position.xyz)+r2.w);
            objectNormal+=w*vec3(dot(r0.xyz,normal),dot(r1.xyz,normal),dot(r2.xyz,normal));
        }
        if (skinNormalize != 0 && dot(objectNormal,objectNormal) > 0.0) objectNormal=normalize(objectNormal);
    }
    vec4 eye=view*world*vec4(objectPosition,1.0);
    vec4 clip=projection*eye;
    gl_Position=vec4(clip.x,-clip.y,2.0*clip.z-clip.w,clip.w);
    eyeDepth=eye.z;
    vertexDiffuse=vertexColor(); vertexSpecular=skinOn != 0 ? vec4(0.0) : specular;
    if (lighting == 0) return;
    vec3 n=transpose(inverse(mat3(view*world)))*objectNormal;
    if (normalizeNormals != 0) n=normalize(n);
    vec4 md=materialColor(materialSources.x,material[0]);
    vec4 ma=materialColor(materialSources.y,material[1]);
    vec4 ms=materialColor(materialSources.z,material[2]);
    vec4 me=materialColor(materialSources.w,material[3]);
    vec3 color=me.rgb+ma.rgb*globalAmbient.rgb;
    vec3 highlight=vec3(0.0);
    for (int i=0;i<8;++i) {
        if (lightType[i]==0) continue;
        vec3 l; float attenuation=1.0;
        if (lightType[i]==3) l=normalize(-(mat3(view)*lightDirection[i]));
        else {
            vec3 delta=(view*vec4(lightPosition[i],1.0)).xyz-eye.xyz;
            float distance=length(delta);
            if (distance>lightSpot[i].x) continue;
            l=normalize(delta);
            attenuation=1.0/dot(lightAttenuation[i],vec3(1.0,distance,distance*distance));
            if (lightType[i]==2) {
                float rho=dot(-l,normalize(mat3(view)*lightDirection[i]));
                float outer=cos(lightSpot[i].w*0.5), inner=cos(lightSpot[i].z*0.5);
                float cone=rho>=inner ? 1.0 : rho<=outer ? 0.0 :
                    pow((rho-outer)/(inner-outer),lightSpot[i].y);
                attenuation*=cone;
            }
        }
        float lambert=max(dot(n,l),0.0);
        color+=attenuation*(ma.rgb*lightAmbient[i].rgb+md.rgb*lightDiffuse[i].rgb*lambert);
        if (specularEnable!=0 && lambert>0.0) {
            vec3 v=localViewer!=0 ? normalize(-eye.xyz) : vec3(0.0,0.0,-1.0);
            float strength=pow(max(dot(n,normalize(l+v)),0.0),power);
            highlight+=attenuation*ms.rgb*lightSpecular[i].rgb*strength;
        }
    }
    vertexDiffuse=vec4(clamp(color,0.0,1.0),md.a);
    vertexSpecular=vec4(clamp(highlight,0.0,1.0),specular.a);
}
)GLSL";

inline constexpr const char* fragment_shader = R"GLSL(#version 330 core
in vec4 vertexDiffuse, vertexSpecular;
in vec2 texcoord[2];
in float eyeDepth;
uniform sampler2D texture0, texture1;
uniform ivec2 colorOp, alphaOp, colorArg1, colorArg2, alphaArg1, alphaArg2, coordinate;
uniform ivec2 textureBound;
uniform vec4 textureFactor;
uniform int alphaTest, alphaFunction, specularEnable;
uniform float alphaReference;
uniform int fogEnable, fogMode, transformed;
uniform vec4 fogColor;
uniform vec3 fogParameters;
out vec4 outputColor;
vec4 argument(int code,vec4 tex,vec4 current) {
    int source=code&15;
    vec4 result=source==0 ? vertexDiffuse : source==1 ? current :
        source==2 ? tex : source==3 ? textureFactor : vertexSpecular;
    if ((code&32)!=0) result=vec4(result.a);
    if ((code&16)!=0) result=vec4(1.0)-result;
    return result;
}
vec4 combine(int op,vec4 a,vec4 b,vec4 tex,vec4 current) {
    if (op==2) return a;
    if (op==3) return b;
    if (op==4) return a*b;
    if (op==5) return 2.0*a*b;
    if (op==6) return 4.0*a*b;
    if (op==7) return a+b;
    if (op==8) return a+b-0.5;
    if (op==9) return 2.0*(a+b-0.5);
    if (op==10) return a-b;
    if (op==11) return a+b*(vec4(1.0)-a);
    float factor=op==12 ? vertexDiffuse.a : op==13 ? tex.a :
                 op==14 ? textureFactor.a : current.a;
    if (op==15) return a+b*(1.0-tex.a);
    return mix(b,a,factor);
}
bool compareAlpha(float value) {
    if (alphaFunction==1) return false;
    if (alphaFunction==2) return value<alphaReference;
    if (alphaFunction==3) return value==alphaReference;
    if (alphaFunction==4) return value<=alphaReference;
    if (alphaFunction==5) return value>alphaReference;
    if (alphaFunction==6) return value!=alphaReference;
    if (alphaFunction==7) return value>=alphaReference;
    return true;
}
void main() {
    vec4 current=vertexDiffuse;
    for (int stage=0;stage<2;++stage) {
        if (colorOp[stage]==1) break;
        if (textureBound[stage]==0 &&
            ((colorOp[stage]!=3 && (colorArg1[stage]&15)==2) ||
             (colorOp[stage]!=2 && (colorArg2[stage]&15)==2))) break;
        vec2 uv=texcoord[coordinate[stage]];
        vec4 tex=textureBound[stage]==0 ? vec4(1.0) :
            stage==0 ? texture(texture0,uv) : texture(texture1,uv);
        vec4 color=combine(colorOp[stage],argument(colorArg1[stage],tex,current),
                           argument(colorArg2[stage],tex,current),tex,current);
        float alpha=alphaOp[stage]==1 ? current.a :
            combine(alphaOp[stage],argument(alphaArg1[stage],tex,current),
                    argument(alphaArg2[stage],tex,current),tex,current).a;
        current=clamp(vec4(color.rgb,alpha),0.0,1.0);
    }
    if (alphaTest!=0 && !compareAlpha(current.a)) discard;
    if (specularEnable!=0) current.rgb=clamp(current.rgb+vertexSpecular.rgb,0.0,1.0);
    if (fogEnable!=0) {
        float factor=transformed!=0 && fogMode==0 ? vertexSpecular.a : 1.0;
        if (fogMode==1) factor=exp(-fogParameters.z*eyeDepth);
        if (fogMode==2) factor=exp(-pow(fogParameters.z*eyeDepth,2.0));
        if (fogMode==3) factor=(fogParameters.y-eyeDepth)/(fogParameters.y-fogParameters.x);
        current.rgb=mix(fogColor.rgb,current.rgb,clamp(factor,0.0,1.0));
    }
    outputColor=current;
}
)GLSL";
// Presentation of a scaled rectangle of a surface (HD mode insets): a quad over the viewport, no vertex attributes.
// `source` is the texel rectangle (x0, y0, x1, y1; row y0 is the top) shown in the viewport.
inline constexpr const char* resample_vertex_shader = R"GLSL(#version 330 core
uniform vec4 source;
out vec2 position;
void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
    position = vec2(mix(source.x, source.z, corner.x), mix(source.w, source.y, corner.y));
}
)GLSL";
// Catmull-Rom resampling in texel space, its kernel widened by the minification factor (`spread`, texels per
// destination pixel, at least 1), normalised and clamped; texels outside the rectangle repeat its edge, so the rest
// of the frame never bleeds in.
inline constexpr const char* resample_fragment_shader = R"GLSL(#version 330 core
uniform sampler2D image;
uniform vec4 source;
uniform vec2 spread;
uniform vec4 shade;
in vec2 position;
out vec4 color;
float cubic(float x) {
    x = abs(x);
    if (x < 1.0) return (1.5 * x - 2.5) * x * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}
void main() {
    ivec2 low = ivec2(ceil(position - 0.5 - 2.0 * spread));
    ivec2 high = ivec2(floor(position - 0.5 + 2.0 * spread));
    ivec2 first = ivec2(source.xy), last = ivec2(source.zw) - 1;
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int y = low.y; y <= high.y; ++y) {
        float wy = cubic((float(y) + 0.5 - position.y) / spread.y);
        for (int x = low.x; x <= high.x; ++x) {
            float w = wy * cubic((float(x) + 0.5 - position.x) / spread.x);
            sum += w * texelFetch(image, clamp(ivec2(x, y), first, last), 0).rgb;
            total += w;
        }
    }
    color = vec4(clamp(sum / total, 0.0, 1.0) * shade.a + shade.rgb, 1.0);
}
)GLSL";
}
