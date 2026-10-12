#version 140

uniform sampler2D WideInputTex;
uniform bool uWideScene;
uniform float uWideRatio;
uniform ivec2 uWideBrightness;
// WideMelon: a BG layer alpha-blended over the 3D (cloud shadows, fog, rain
// tint), continued into the expanded view's sides
uniform bool uWideOverlay;
uniform sampler2D WideOverlayTex;
uniform vec2 uOverlaySize;
uniform vec2 uOverlayScroll;
uniform ivec2 uOverlayBlend;
uniform sampler2D MainInputTexA;
uniform sampler2D MainInputTexB;
uniform sampler2DArray AuxInputTex;
// WideMelon: weather sprites (rain, snow), drawn into the expanded view's sides:
// for each, its sprite (in the pre-rendered sprite atlas), position, and size
// and flips (width | height << 8 | hflip << 16 | vflip << 17)
uniform int uWeatherCount;
uniform ivec4 uWeather[64];
uniform bool uWeatherOverOverlay;
uniform sampler2D WeatherTex;

// the weather at a DS position outside the screen, or alpha 0
vec4 Weather(ivec2 p)
{
    for (int i = 0; i < uWeatherCount; i++)
    {
        ivec4 s = uWeather[i];
        ivec2 size = ivec2(s.w & 0xFF, (s.w >> 8) & 0xFF);
        // Y over 127 also counts from the top (the DS wraps it)
        int ly = p.y - s.z;
        if (ly < 0 || ly >= size.y) ly = p.y - (s.z & 0xFF);
        if (ly < 0 || ly >= size.y) continue;
        for (int o = -256; o <= 256; o += 256)
        {
            int lx = p.x - (s.y + o);
            if (lx < 0 || lx >= size.x) continue;
            ivec2 t = ivec2(((s.w & (1 << 16)) != 0) ? size.x - 1 - lx : lx,
                            ((s.w & (1 << 17)) != 0) ? size.y - 1 - ly : ly);
            vec4 c = texelFetch(WeatherTex, ivec2((s.x & 0xF) * 64, (s.x >> 4) * 64) + t, 0);
            if (c.a > 0.0) return c;
        }
    }
    return vec4(0.0);
}

layout(std140) uniform ubFinalPassConfig
{
    bvec4 uScreenSwap[48]; // one bool per scanline
    int uScaleFactor;
    int uAuxLayer;
    int uDispModeA;
    int uDispModeB;
    int uBrightModeA;
    int uBrightModeB;
    int uBrightFactorA;
    int uBrightFactorB;
    float uAuxColorFactor;
};

smooth in vec3 fTexcoord;

out vec4 oTopColor;
out vec4 oBottomColor;

ivec3 MasterBrightness(ivec3 color, int brightmode, int evy)
{
    if (brightmode == 1)
    {
        // up
        color += (((0x3F - color) * evy) >> 4);
    }
    else if (brightmode == 2)
    {
        // down
        color -= (((color * evy) + 0xF) >> 4);
    }

    return color;
}

void main()
{
    vec2 nativeCoord = vec2((fTexcoord.x - 0.5) * uWideRatio + 0.5, fTexcoord.y);
    bool wing = nativeCoord.x < 0.0 || nativeCoord.x >= 1.0;
    ivec4 col_main = ivec4(texture(MainInputTexA, nativeCoord, 0) * 255.0) >> 2;
    ivec4 col_sub = ivec4(texture(MainInputTexB, nativeCoord, 0) * 255.0) >> 2;
    if (wing) {
        col_main = uWideScene ? ivec4(texture(WideInputTex, fTexcoord.xy) * 255.0) >> 2 : ivec4(0);
        vec4 weather = vec4(0.0);
        if (uWideScene && uWeatherCount > 0)
            weather = Weather(ivec2(floor(vec2(nativeCoord.x * 256.0, fTexcoord.y * 192.0))));
        if (weather.a > 0.0 && !uWeatherOverOverlay)
            col_main.rgb = ivec3(weather.rgb * 255.0) >> 2;
        col_sub = ivec4(0);
        if (uWideScene && uWideOverlay)
        {
            // the layer where it would be if the DS screen were this wide
            // (its tile map wraps around), blended like the DS does
            vec2 bgpos = vec2(nativeCoord.x * 256.0, fTexcoord.y * 192.0) + uOverlayScroll;
            vec4 overlay = texture(WideOverlayTex, bgpos / uOverlaySize);
            if (overlay.a > 0.0)
            {
                ivec3 o = ivec3(overlay.rgb * 255.0) >> 2;
                col_main.rgb = min(((o * uOverlayBlend.x) + (col_main.rgb * uOverlayBlend.y) + 0x8) >> 4, ivec3(0x3F));
            }
        }
        if (weather.a > 0.0 && uWeatherOverOverlay)
            col_main.rgb = ivec3(weather.rgb * 255.0) >> 2;
        if (uWideScene && uWideBrightness.x == 2)
            col_main += (((63 - col_main) * uWideBrightness.y + 8) >> 4);
        else if (uWideScene && uWideBrightness.x == 3)
            col_main -= ((col_main * uWideBrightness.y + 7) >> 4);
    }

    ivec3 output_main, output_sub;

    if (uDispModeA == 0)
    {
        // screen disabled (white)
        output_main = ivec3(63, 63, 63);
    }
    else if (uDispModeA == 1)
    {
        // BG/OBJ layers
        output_main = col_main.rgb;
    }
    else
    {
        // VRAM display / mainmem FIFO
        output_main = ivec3(texture(AuxInputTex, vec3(nativeCoord.x, fTexcoord.z, uAuxLayer)).rgb * uAuxColorFactor);
    }

    if (uDispModeB == 0)
    {
        // screen disabled (white)
        output_sub = ivec3(63, 63, 63);
    }
    else
    {
        // BG/OBJ layers
        output_sub = col_sub.rgb;
    }

    if (wing && !uWideScene) output_main = ivec3(0);
    if (wing) output_sub = ivec3(0);

    if (uDispModeA != 0)
        output_main = MasterBrightness(output_main, uBrightModeA, uBrightFactorA);
    if (uDispModeB != 0)
        output_sub = MasterBrightness(output_sub, uBrightModeB, uBrightFactorB);

    output_main = (output_main << 2) | (output_main >> 6);
    output_sub = (output_sub << 2) | (output_sub >> 6);

    int line = int(fTexcoord.y * 192);
    bool swapbit = uScreenSwap[line>>2][line&0x3];

    if (!swapbit)
    {
        oTopColor = vec4(vec3(output_sub) / 255.0, 1.0);
        oBottomColor = vec4(vec3(output_main) / 255.0, 1.0);
    }
    else
    {
        oTopColor = vec4(vec3(output_main) / 255.0, 1.0);
        oBottomColor = vec4(vec3(output_sub) / 255.0, 1.0);
    }
}
