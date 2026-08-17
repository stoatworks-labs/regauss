#include "../Shaders.h"

namespace regauss::shaders
{
/// Note what is NOT here: `uv = vUV * MaxUV`, which is what every other FFGL
/// plugin in the fleet writes. See the comment in Shaders.h -- this plugin
/// warps, so the scale has to happen after the clamp, in the fragment shader.
const char* const kVertex = R"(#version 410 core
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";
} // namespace regauss::shaders
