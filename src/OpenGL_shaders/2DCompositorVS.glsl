#version 140

uniform int uScaleFactor;
// WideMelon: when not 0, the part past the screen's right edge is composited,
// uWingSpan pixels starting at uWingX
uniform int uWingX;
uniform int uWingSpan;

in vec2 vPosition;

smooth out vec4 fTexcoord;

void main()
{
    gl_Position = vec4((vPosition * 2) - 1, 0, 1);
    if (uWingX != 0)
        fTexcoord.xy = vPosition * vec2(uWingSpan, 192) + vec2(uWingX, 0);
    else
        fTexcoord.xy = vPosition * vec2(256, 192);
    fTexcoord.zw = fTexcoord.xy * uScaleFactor;
}
