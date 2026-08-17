#pragma once

#include <FFGLSDK.h>

namespace regauss
{
/**
    An off-screen buffer for one stage of the chain.

    Two things on top of the SDK's FFGLFBO.

    **It reallocates only when it has to.** Every buffer here is sized by the
    composition -- a magnetic field is a fact about the room rather than about
    a broadcast standard, so unlike old-cathode there is no authentic raster to
    snap to. Ensure() is called every frame and is a no-op in the overwhelming
    majority of them.

    **It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
    deletes the framebuffer and the depth renderbuffer, then tests
    `depthBufferID` a second time where it plainly meant `colorTextureID` --
    so the colour texture is leaked on every release (SDK b1afaf9,
    `FFGLFBO.cpp`). One leak would not matter; this plugin drops and rebuilds
    every buffer whenever the composition size changes, and somebody dragging a
    resolution slider about should not be paying megabytes of texture memory
    for each position it passes through.
*/
class PassBuffer : public ffglex::FFGLFBO
{
public:
	~PassBuffer();

	/// Allocate at this size and format, reusing the existing buffer if it
	/// already matches. Newly allocated buffers are cleared, because a pass that
	/// reads its own history reads this on its first frame.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format );

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	bool IsValid() const
	{
		return GetGLID() != 0;
	}
};

} // namespace regauss
