#include <gccore.h>
#include <ogc/gu.h>
#include <malloc.h>

#include "graphics/GLInterface.h"
#include "graphics/GLImage.h"
#include "SexyAppBase.h"
#include "misc/AutoCrit.h"
#include "misc/CritSect.h"
#include "graphics/Graphics.h"
#include "graphics/MemoryImage.h"

#define GetColorFromTriVertex(theVertex, theColor) (theVertex.color?theVertex.color:theColor)

#define DEFAULT_FIFO_SIZE (256*1024)

using namespace Sexy;

static int gMinTextureWidth;
static int gMinTextureHeight;
static int gMaxTextureWidth;
static int gMaxTextureHeight;
static int gSupportedPixelFormats;
static bool gTextureSizeMustBePow2;
static const int MAX_TEXTURE_SIZE = 1024;
static bool gLinearFilter = false;

///////////////////////////////////////////////////////////////////////////////
// GX display/video state
///////////////////////////////////////////////////////////////////////////////

static GXRModeObj* gRMode;
static void* gXfb[2];
static u8 gFbIndex = 0;
static void* gGpFifo;
static Mtx44 gProjection;

///////////////////////////////////////////////////////////////////////////////
// Unlike citro3d's C3D_DrawArrays (which needs a pre-filled vertex buffer),
// GX_Begin/GX_End write directly into the command FIFO, so there is no need
// to stage vertices into an intermediate array like the 3DS/PC backends do -
// each draw call just needs to know its vertex count up front.
///////////////////////////////////////////////////////////////////////////////

static bool gCurrentlyTextured = false;

static void SetTexture(GXTexObj *theTex, bool force = false)
{
	bool textured = (theTex != NULL);

	if (force || textured != gCurrentlyTextured)
	{
		gCurrentlyTextured = textured;

		GX_SetVtxDesc(GX_VA_TEX0, textured ? GX_DIRECT : GX_NONE);
		GX_SetNumTexGens(textured ? 1 : 0);

		if (textured)
		{
			GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
			GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE); // texture * vertex color
		}
		else
		{
			GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR); // pass through vertex color
		}
	}

	if (textured)
		GX_LoadTexObj(theTex, GX_TEXMAP0);
}

static void GxDrawVerts(u8 thePrimitive, const GLVertex *theVerts, int theCount, bool theTextured)
{
	if (theCount <= 0)
		return;

	GX_Begin(thePrimitive, GX_VTXFMT0, theCount);
	for (int i = 0; i < theCount; i++)
	{
		const GLVertex &v = theVerts[i];
		GX_Position3f32(v.sx, v.sy, v.sz);
		GX_Color4u8((v.color >> 0) & 0xFF, (v.color >> 8) & 0xFF, (v.color >> 16) & 0xFF, (v.color >> 24) & 0xFF);
		if (theTextured)
			GX_TexCoord2f32(v.tu, v.tv);
	}
	GX_End();
}

static void GxDrawVerts(u8 thePrimitive, VertexList &theVerts, bool theTextured)
{
	if (theVerts.size() <= 0)
		return;

	GxDrawVerts(thePrimitive, &theVerts[0], theVerts.size(), theTextured);
}

///////////////////////////////////////////////////////////////////////////////
// GX texture tiling
//
// Unlike the 3DS's 8x8 Z-order (Morton) tiles, GX textures use plain 4x4
// raster-order tiles (for the 16/32bpp formats used here); RGBA8 additionally
// splits each 4x4 tile into two 32-byte "cache lines" - AR bytes then GB bytes
// for the same 16 texels - instead of one contiguous 64-byte block.
///////////////////////////////////////////////////////////////////////////////
static void ToTiledTexture16(void *dst, const uint16_t *src, int width, int height)
{
	uint16_t *out = (uint16_t*)dst;

	for (int ty = 0; ty < height; ty += 4)
	{
		for (int tx = 0; tx < width; tx += 4)
		{
			for (int y = 0; y < 4; y++)
			{
				const uint16_t *srcRow = src + (ty + y) * width + tx;
				for (int x = 0; x < 4; x++)
					*out++ = srcRow[x];
			}
		}
	}
}

static void ToTiledTextureRGBA8(void *dst, const uint32_t *src, int width, int height)
{
	uint8_t *out = (uint8_t*)dst;

	for (int ty = 0; ty < height; ty += 4)
	{
		for (int tx = 0; tx < width; tx += 4)
		{
			uint8_t *arBlock = out;
			uint8_t *gbBlock = out + 32;

			for (int y = 0; y < 4; y++)
			{
				const uint32_t *srcRow = src + (ty + y) * width + tx;
				for (int x = 0; x < 4; x++)
				{
					uint32_t pixel = srcRow[x];
					uint8_t r = pixel & 0xff;
					uint8_t g = (pixel >> 8) & 0xff;
					uint8_t b = (pixel >> 16) & 0xff;
					uint8_t a = (pixel >> 24) & 0xff;

					*arBlock++ = a;
					*arBlock++ = r;
					*gbBlock++ = g;
					*gbBlock++ = b;
				}
			}

			out += 64;
		}
	}
}

static inline uint16_t PackRGB5A3(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	if (a == 0xff)
		return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
	else
		return ((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
}

static void CopyImageToTexture8888(void *dst, MemoryImage *theImage, int offx, int offy, int theWidth, int theHeight, int theDestPitch, int theDestHeight, bool rightPad, bool bottomPad)
{
	uint32_t *aDest = new uint32_t[theDestPitch * theDestHeight];

	if (theImage->mColorTable == NULL)
	{
		uint32_t *srcRow = (uint32_t*)theImage->GetBits() + offy * theImage->GetWidth() + offx;
		uint32_t *dstRow = aDest;

		for (int y = 0; y < theHeight; y++)
		{
			uint32_t *src = srcRow;
			uint32_t *dst = dstRow;
			for (int x = 0; x < theWidth; x++)
				*dst++ = *src++;

			if (rightPad)
				*dst = *(dst - 1);

			srcRow += theImage->GetWidth();
			dstRow += theDestPitch;
		}
	}
	else // palette
	{
		uint8_t *srcRow = (uint8_t*)theImage->mColorIndices + offy * theImage->GetWidth() + offx;
		uint32_t *dstRow = aDest;
		uint32_t *palette = (uint32_t*)theImage->mColorTable;

		for (int y = 0; y < theHeight; y++)
		{
			uint8_t *src = srcRow;
			uint32_t *dst = dstRow;
			for (int x = 0; x < theWidth; x++)
				*dst++ = palette[*src++];

			if (rightPad)
				*dst = *(dst - 1);

			srcRow += theImage->GetWidth();
			dstRow += theDestPitch;
		}
	}

	if (bottomPad)
	{
		uint32_t *dstrow = aDest + (theDestPitch * theHeight);
		memcpy(dstrow, dstrow - (theDestPitch * 4), (theDestPitch * 4));
	}

	ToTiledTextureRGBA8(dst, aDest, theDestPitch, theDestHeight);

	delete[] aDest;
}

static void CopyImageToTextureRGB5A3(void *dst, MemoryImage *theImage, int offx, int offy, int theWidth, int theHeight, int theDestPitch, int theDestHeight, bool rightPad, bool bottomPad)
{
	uint16_t *aDest = new uint16_t[theDestPitch * theDestHeight];

	if (theImage->mColorTable == NULL)
	{
		uint32_t *srcRow = (uint32_t*)theImage->GetBits() + offy * theImage->GetWidth() + offx;
		uint16_t *dstRow = aDest;

		for (int y = 0; y < theHeight; y++)
		{
			uint32_t *src = srcRow;
			uint16_t *dst = dstRow;
			for (int x = 0; x < theWidth; x++)
			{
				uint32_t aPixel = *src++;
				*dst++ = PackRGB5A3(aPixel & 0xff, (aPixel >> 8) & 0xff, (aPixel >> 16) & 0xff, (aPixel >> 24) & 0xff);
			}

			if (rightPad)
				*dst = *(dst - 1);

			srcRow += theImage->GetWidth();
			dstRow += theDestPitch;
		}
	}
	else // palette
	{
		uint8_t *srcRow = (uint8_t*)theImage->mColorIndices + offy * theImage->GetWidth() + offx;
		uint16_t *dstRow = aDest;
		uint32_t *palette = (uint32_t*)theImage->mColorTable;

		for (int y = 0; y < theHeight; y++)
		{
			uint8_t *src = srcRow;
			uint16_t *dst = dstRow;
			for (int x = 0; x < theWidth; x++)
			{
				uint32_t aPixel = palette[*src++];
				*dst++ = PackRGB5A3(aPixel & 0xff, (aPixel >> 8) & 0xff, (aPixel >> 16) & 0xff, (aPixel >> 24) & 0xff);
			}

			if (rightPad)
				*dst = *(dst - 1);

			srcRow += theImage->GetWidth();
			dstRow += theDestPitch;
		}
	}

	if (bottomPad)
	{
		uint16_t *dstrow = aDest + (theDestPitch * theHeight);
		memcpy(dstrow, dstrow - (theDestPitch * 2), (theDestPitch * 2));
	}

	ToTiledTexture16(dst, aDest, theDestPitch, theDestHeight);

	delete[] aDest;
}

static void CopyImageToTexture565(void *dst, MemoryImage *theImage, int offx, int offy, int theWidth, int theHeight, int theDestPitch, int theDestHeight, bool rightPad, bool bottomPad)
{
	uint16_t *aDest = new uint16_t[theDestPitch * theDestHeight];

	if (theImage->mColorTable == NULL)
	{
		uint32_t *srcRow = (uint32_t*)theImage->GetBits() + offy * theImage->GetWidth() + offx;
		uint16_t *dstRow = aDest;

		for (int y = 0; y < theHeight; y++)
		{
			uint32_t *src = srcRow;
			uint16_t *dst = dstRow;
			for (int x = 0; x < theWidth; x++)
			{
				uint32_t aPixel = *src++;
				*dst++ = ((aPixel >> 8) & 0xF800) | ((aPixel >> 5) & 0x07E0) | ((aPixel >> 3) & 0x001F);
			}

			if (rightPad)
				*dst = *(dst - 1);

			srcRow += theImage->GetWidth();
			dstRow += theDestPitch;
		}
	}
	else
	{
		uint8_t *srcRow = (uint8_t*)theImage->mColorIndices + offy * theImage->GetWidth() + offx;
		uint16_t *dstRow = aDest;
		uint32_t *palette = (uint32_t*)theImage->mColorTable;

		for (int y = 0; y < theHeight; y++)
		{
			uint8_t *src = srcRow;
			uint16_t *dst = dstRow;
			for (int x = 0; x < theWidth; x++)
			{
				uint32_t aPixel = palette[*src++];
				*dst++ = ((aPixel >> 8) & 0xF800) | ((aPixel >> 5) & 0x07E0) | ((aPixel >> 3) & 0x001F);
			}

			if (rightPad)
				*dst = *(dst - 1);

			srcRow += theImage->GetWidth();
			dstRow += theDestPitch;
		}
	}

	if (bottomPad)
	{
		uint16_t *dstrow = aDest + (theDestPitch * theHeight);
		memcpy(dstrow, dstrow - (theDestPitch * 2), (theDestPitch * 2));
	}

	ToTiledTexture16(dst, aDest, theDestPitch, theDestHeight);

	delete[] aDest;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
static void CopyImageToTexture(void *dst, MemoryImage *theImage, int offx, int offy, int texWidth, int texHeight, PixelFormat theFormat)
{
	int aWidth = std::min(texWidth, (theImage->GetWidth() - offx));
	int aHeight = std::min(texHeight, (theImage->GetHeight() - offy));

	bool rightPad = aWidth < texWidth;
	bool bottomPad = aHeight < texHeight;

	if (aWidth > 0 && aHeight > 0)
	{
		switch (theFormat)
		{
			case PixelFormat_A8R8G8B8:	CopyImageToTexture8888(dst, theImage, offx, offy, aWidth, aHeight, texWidth, texHeight, rightPad, bottomPad); break;
			case PixelFormat_A4R4G4B4:	CopyImageToTextureRGB5A3(dst, theImage, offx, offy, aWidth, aHeight, texWidth, texHeight, rightPad, bottomPad); break;
			case PixelFormat_R5G6B5:	CopyImageToTexture565(dst, theImage, offx, offy, aWidth, aHeight, texWidth, texHeight, rightPad, bottomPad); break;
			case PixelFormat_Palette8:	break; // never selected below, same as the 3DS backend
			case PixelFormat_Unknown: break;
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
static int GetClosestPowerOf2Above(int theNum)
{
	int aPower2 = 1;
	while (aPower2 < theNum)
		aPower2 <<= 1;
	return aPower2;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
static bool IsPowerOf2(int theNum)
{
	int aNumBits = 0;
	while (theNum > 0)
	{
		aNumBits += theNum & 1;
		theNum >>= 1;
	}

	return aNumBits == 1;
}
///////////////////////////////////////////////////////////////////////////////
// Every value this function can return is an exact power of two (>=
// gMinTextureWidth/Height, which is 8), which conveniently always satisfies
// GX's 4x4 tiling alignment requirement without any extra padding logic.
///////////////////////////////////////////////////////////////////////////////
static void GetBestTextureDimensions(int &theWidth, int &theHeight, bool isEdge, bool usePow2, uint32_t theImageFlags)
{
	if (theImageFlags & D3DImageFlag_Use64By64Subdivisions)
	{
		theWidth = theHeight = 64;
		return;
	}

	static int aGoodTextureSize[MAX_TEXTURE_SIZE];
	static bool haveInited = false;
	if (!haveInited)
	{
		haveInited = true;
		int i;
		int aPow2 = 1;
		for (i = 0; i < MAX_TEXTURE_SIZE; i++)
		{
			if (i > aPow2)
				aPow2 <<= 1;

			int aGoodValue = aPow2;
			if ((aGoodValue - i) > 64)
			{
				aGoodValue >>= 1;
				while (true)
				{
					int aLeftOver = i % aGoodValue;
					if (aLeftOver < 64 || IsPowerOf2(aLeftOver))
						break;

					aGoodValue >>= 1;
				}
			}
			aGoodTextureSize[i] = aGoodValue;
		}
	}

	int aWidth = theWidth;
	int aHeight = theHeight;

	if (usePow2)
	{
		if (isEdge || (theImageFlags & D3DImageFlag_MinimizeNumSubdivisions))
		{
			aWidth = aWidth >= gMaxTextureWidth ? gMaxTextureWidth : GetClosestPowerOf2Above(aWidth);
			aHeight = aHeight >= gMaxTextureHeight ? gMaxTextureHeight : GetClosestPowerOf2Above(aHeight);
		}
		else
		{
			aWidth = aWidth >= gMaxTextureWidth ? gMaxTextureWidth : aGoodTextureSize[aWidth];
			aHeight = aHeight >= gMaxTextureHeight ? gMaxTextureHeight : aGoodTextureSize[aHeight];
		}
	}

	if (aWidth < gMinTextureWidth)
		aWidth = gMinTextureWidth;

	if (aHeight < gMinTextureHeight)
		aHeight = gMinTextureHeight;

	theWidth = aWidth;
	theHeight = aHeight;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
TextureData::TextureData()
{
	mWidth = 0;
	mHeight = 0;
	mTexVecWidth = 0;
	mTexVecHeight = 0;
	mBitsChangedCount = 0;
	mTexMemSize = 0;
	mTexPieceWidth = 64;
	mTexPieceHeight = 64;

	mPixelFormat = PixelFormat_Unknown;
	mImageFlags = 0;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
TextureData::~TextureData()
{
	ReleaseTextures();
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void TextureData::ReleaseTextures()
{
	for (int i = 0; i < (int)mTextures.size(); i++)
	{
		if (mTextures[i].mTexData)
			free(mTextures[i].mTexData);
	}

	mTextures.clear();

	mTexMemSize = 0;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void TextureData::CreateTextureDimensions(MemoryImage *theImage)
{
	int aWidth = theImage->GetWidth();
	int aHeight = theImage->GetHeight();
	unsigned int i;

	// Calculate inner piece sizes
	mTexPieceWidth = aWidth;
	mTexPieceHeight = aHeight;
	bool usePow2 = true;
	GetBestTextureDimensions(mTexPieceWidth, mTexPieceHeight, false, usePow2, mImageFlags);

	// Calculate right boundary piece sizes
	int aRightWidth = aWidth % mTexPieceWidth;
	int aRightHeight = mTexPieceHeight;
	if (aRightWidth > 0)
		GetBestTextureDimensions(aRightWidth, aRightHeight, true, usePow2, mImageFlags);
	else
		aRightWidth = mTexPieceWidth;

	// Calculate bottom boundary piece sizes
	int aBottomWidth = mTexPieceWidth;
	int aBottomHeight = aHeight % mTexPieceHeight;
	if (aBottomHeight > 0)
		GetBestTextureDimensions(aBottomWidth, aBottomHeight, true, usePow2, mImageFlags);
	else
		aBottomHeight = mTexPieceHeight;

	// Calculate corner piece size
	int aCornerWidth = aRightWidth;
	int aCornerHeight = aBottomHeight;
	GetBestTextureDimensions(aCornerWidth, aCornerHeight, true, usePow2, mImageFlags);

	// Allocate texture array
	mTexVecWidth = (aWidth + mTexPieceWidth - 1) / mTexPieceWidth;
	mTexVecHeight = (aHeight + mTexPieceHeight - 1) / mTexPieceHeight;
	mTextures.resize(mTexVecWidth * mTexVecHeight);

	// Assign inner pieces
	for (i = 0; i < mTextures.size(); i++)
	{
		TextureDataPiece& aPiece = mTextures[i];
		aPiece.mWidth = mTexPieceWidth;
		aPiece.mHeight = mTexPieceHeight;
		aPiece.mTexData = NULL;
	}

	// Assign right pieces
	for (i = mTexVecWidth - 1; i < mTextures.size(); i += mTexVecWidth)
	{
		TextureDataPiece& aPiece = mTextures[i];
		aPiece.mWidth = aRightWidth;
		aPiece.mHeight = aRightHeight;
	}

	// Assign bottom pieces
	for (i = mTexVecWidth * (mTexVecHeight - 1); i < mTextures.size(); i++)
	{
		TextureDataPiece& aPiece = mTextures[i];
		aPiece.mWidth = aBottomWidth;
		aPiece.mHeight = aBottomHeight;
	}

	// Assign corner piece
	mTextures.back().mWidth = aCornerWidth;
	mTextures.back().mHeight = aCornerHeight;

	mMaxTotalU = aWidth / (float)mTexPieceWidth;
	mMaxTotalV = aHeight / (float)mTexPieceHeight;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void TextureData::CreateTextures(MemoryImage *theImage)
{
	theImage->DeleteSWBuffers(); // don't need these buffers for 3d drawing

	// Choose appropriate pixel format
	PixelFormat aFormat = PixelFormat_A8R8G8B8;

	theImage->CommitBits();
	if (!theImage->mHasAlpha && !theImage->mHasTrans && (gSupportedPixelFormats & PixelFormat_R5G6B5))
	{
		if (!(theImage->mD3DFlags & D3DImageFlag_UseA8R8G8B8))
			aFormat = PixelFormat_R5G6B5;
	}

	if ((theImage->mD3DFlags & D3DImageFlag_UseA4R4G4B4) && aFormat==PixelFormat_A8R8G8B8 && (gSupportedPixelFormats & PixelFormat_A4R4G4B4))
		aFormat = PixelFormat_A4R4G4B4;

	if (aFormat==PixelFormat_A8R8G8B8 && !(gSupportedPixelFormats & PixelFormat_A8R8G8B8))
		aFormat = PixelFormat_A4R4G4B4;

	// Release texture if image size has changed
	bool createTextures = false;
	if (mWidth!=theImage->mWidth || mHeight!=theImage->mHeight || aFormat!=mPixelFormat || theImage->mD3DFlags!=mImageFlags)
	{
		ReleaseTextures();

		mPixelFormat = aFormat;
		mImageFlags = theImage->mD3DFlags;
		CreateTextureDimensions(theImage);
		createTextures = true;
	}

	int i,x,y;

	int aHeight = theImage->GetHeight();
	int aWidth = theImage->GetWidth();

	int aFormatSize = 4;
	u8 aFormatType = GX_TF_RGBA8;
	if (aFormat==PixelFormat_R5G6B5)
	{
		aFormatSize = 2;
		aFormatType = GX_TF_RGB565;
	}
	else if (aFormat==PixelFormat_A4R4G4B4)
	{
		aFormatSize = 2;
		aFormatType = GX_TF_RGB5A3;
	}

	i=0;
	for(y=0; y<aHeight; y+=mTexPieceHeight)
	{
		for(x=0; x<aWidth; x+=mTexPieceWidth, i++)
		{
			TextureDataPiece &aPiece = mTextures[i];
			int aDataSize = aPiece.mWidth * aPiece.mHeight * aFormatSize;

			if (createTextures)
			{
				aPiece.mTexData = memalign(32, aDataSize);
				mTexMemSize += aDataSize;
			}

			CopyImageToTexture(aPiece.mTexData, theImage, x, y, aPiece.mWidth, aPiece.mHeight, aFormat);
			DCFlushRange(aPiece.mTexData, aDataSize);

			GX_InitTexObj(&aPiece.mTexture, aPiece.mTexData, aPiece.mWidth, aPiece.mHeight, aFormatType, GX_CLAMP, GX_CLAMP, GX_FALSE);
			u8 aFilter = gLinearFilter ? GX_LINEAR : GX_NEAR;
			GX_InitTexObjFilterMode(&aPiece.mTexture, aFilter, aFilter);
		}
	}

	mWidth = theImage->mWidth;
	mHeight = theImage->mHeight;
	mBitsChangedCount = theImage->mBitsChangedCount;
	mPixelFormat = aFormat;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void TextureData::CheckCreateTextures(MemoryImage *theImage)
{
	if(mPixelFormat==PixelFormat_Unknown || theImage->mWidth != mWidth || theImage->mHeight != mHeight || theImage->mBitsChangedCount != mBitsChangedCount || theImage->mD3DFlags != mImageFlags)
		CreateTextures(theImage);
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
GXTexObj* TextureData::GetTexture(int x, int y, int &width, int &height, float &u1, float &v1, float &u2, float &v2)
{
	int tx = x/mTexPieceWidth;
	int ty = y/mTexPieceHeight;

	TextureDataPiece &aPiece = mTextures[ty*mTexVecWidth + tx];

	int left = x%mTexPieceWidth;
	int top = y%mTexPieceHeight;
	int right = left+width;
	int bottom = top+height;

	if(right > aPiece.mWidth)
		right = aPiece.mWidth;

	if(bottom > aPiece.mHeight)
		bottom = aPiece.mHeight;

	width = right-left;
	height = bottom-top;

	u1 = (float)left/aPiece.mWidth;
	v1 = (float)top/aPiece.mHeight;
	u2 = (float)right/aPiece.mWidth;
	v2 = (float)bottom/aPiece.mHeight;

	return &aPiece.mTexture;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
GXTexObj* TextureData::GetTextureF(float x, float y, float &width, float &height, float &u1, float &v1, float &u2, float &v2)
{
	int tx = x/mTexPieceWidth;
	int ty = y/mTexPieceHeight;

	TextureDataPiece &aPiece = mTextures[ty*mTexVecWidth + tx];

	float left = x - tx*mTexPieceWidth;
	float top = y - ty*mTexPieceHeight;
	float right = left+width;
	float bottom = top+height;

	if(right > aPiece.mWidth)
		right = aPiece.mWidth;

	if(bottom > aPiece.mHeight)
		bottom = aPiece.mHeight;

	width = right-left;
	height = bottom-top;

	u1 = (float)left/aPiece.mWidth;
	v1 = (float)top/aPiece.mHeight;
	u2 = (float)right/aPiece.mWidth;
	v2 = (float)bottom/aPiece.mHeight;

	return &aPiece.mTexture;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
static void SetLinearFilter(bool linear)
{
	gLinearFilter = linear;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void TextureData::Blt(float theX, float theY, const Rect& theSrcRect, const Color& theColor)
{
	int srcLeft = theSrcRect.mX;
	int srcTop = theSrcRect.mY;
	int srcRight = srcLeft + theSrcRect.mWidth;
	int srcBottom = srcTop + theSrcRect.mHeight;
	int srcX, srcY;
	float dstX, dstY;
	int aWidth,aHeight;
	float u1,v1,u2,v2;

	srcY = srcTop;
	dstY = theY;

	uint32_t aColor = (theColor.mRed << 0) | (theColor.mGreen << 8) | (theColor.mBlue << 16) | (theColor.mAlpha << 24);

	if ((srcLeft >= srcRight) || (srcTop >= srcBottom))
		return;

	while(srcY < srcBottom)
	{
		srcX = srcLeft;
		dstX = theX;
		while(srcX < srcRight)
		{
			aWidth = srcRight-srcX;
			aHeight = srcBottom-srcY;
			GXTexObj* aTexture = GetTexture(srcX, srcY, aWidth, aHeight, u1, v1, u2, v2);

			float x = dstX - 0.5f;
			float y = dstY - 0.5f;

			GLVertex aVertex[4] = {
				{ {x},        {y},         {0},{aColor},{u1},{v1} },
				{ {x},        {y+aHeight}, {0},{aColor},{u1},{v2} },
				{ {x+aWidth}, {y+aHeight}, {0},{aColor},{u2},{v2} },
				{ {x+aWidth}, {y},         {0},{aColor},{u2},{v1} }
			};

			SetTexture(aTexture);
			GxDrawVerts(GX_QUADS, aVertex, 4, true);

			srcX += aWidth;
			dstX += aWidth;
		}

		srcY += aHeight;
		dstY += aHeight;
	}
}

static inline float GetCoord(const GLVertex& theVertex, int theCoord)
{
	switch (theCoord)
	{
	case 0: return theVertex.sx;
	case 1: return theVertex.sy;
	case 2: return theVertex.sz;
	case 3: return theVertex.tu;
	case 4: return theVertex.tv;
	default: return 0;
	}
}

static inline GLVertex Interpolate(const GLVertex &v1, const GLVertex &v2, float t)
{
	GLVertex aVertex = v1;
	aVertex.sx = v1.sx + t*(v2.sx-v1.sx);
	aVertex.sy = v1.sy + t*(v2.sy-v1.sy);
	aVertex.tu = v1.tu + t*(v2.tu-v1.tu);
	aVertex.tv = v1.tv + t*(v2.tv-v1.tv);
	if (v1.color!=v2.color)
	{
		int r = ((v1.color >> 0) & 0xff) + t*( ((v2.color >> 0) & 0xff) - ((v1.color >> 0) & 0xff) );
		int g = ((v1.color >> 8) & 0xff) + t*( ((v2.color >> 8) & 0xff) - ((v1.color >> 8) & 0xff) );
		int b = ((v1.color >> 16) & 0xff) + t*( ((v2.color >> 16) & 0xff) - ((v1.color >> 16) & 0xff) );
		int a = ((v1.color >> 24) & 0xff) + t*( ((v2.color >> 24) & 0xff) - ((v1.color >> 24) & 0xff) );
		aVertex.color = (r << 0) | (g << 8) | (b << 16) | (a << 24);
	}

	return aVertex;
}

template<class Pred>
struct PointClipper
{
	Pred mPred;

	void ClipPoint(int n, float clipVal, const GLVertex& v1, const GLVertex& v2, VertexList& out)
	{
		if (!mPred(GetCoord(v1, n), clipVal))
		{
			if (!mPred(GetCoord(v2, n), clipVal)) // both inside
				out.push_back(v2);
			else // inside -> outside
			{
				float t = (clipVal - GetCoord(v1, n)) / (GetCoord(v2, n) - GetCoord(v1, n));
				out.push_back(Interpolate(v1, v2, t));
			}
		}
		else
		{
			if (!mPred(GetCoord(v2, n), clipVal)) // outside -> inside
			{
				float t = (clipVal - GetCoord(v1, n)) / (GetCoord(v2, n) - GetCoord(v1, n));
				out.push_back(Interpolate(v1, v2, t));
				out.push_back(v2);
			}
			//			else // outside -> outside
		}
	}

	void ClipPoints(int n, float clipVal, VertexList& in, VertexList& out)
	{
		if (in.size() < 2)
			return;

		ClipPoint(n, clipVal, in[in.size() - 1], in[0], out);
		for (VertexList::size_type i = 0; i < in.size() - 1; i++)
			ClipPoint(n, clipVal, in[i], in[i + 1], out);
	}
};

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
static void DrawPolyClipped(const Rect *theClipRect, const VertexList &theList, bool theTextured)
{
	VertexList l1, l2;
	l1 = theList;

	int left = theClipRect->mX;
	int right = left + theClipRect->mWidth;
	int top = theClipRect->mY;
	int bottom = top + theClipRect->mHeight;

	VertexList *in = &l1, *out = &l2;
	PointClipper<std::less<float> > aLessClipper;
	PointClipper<std::greater_equal<float> > aGreaterClipper;

	aLessClipper.ClipPoints(0,left,*in,*out); std::swap(in,out); out->clear();
	aLessClipper.ClipPoints(1,top,*in,*out); std::swap(in,out); out->clear();
	aGreaterClipper.ClipPoints(0,right,*in,*out); std::swap(in,out); out->clear();
	aGreaterClipper.ClipPoints(1,bottom,*in,*out);

	VertexList &aList = *out;

	if (aList.size() >= 3)
		GxDrawVerts(GX_TRIANGLEFAN, aList, theTextured);
}


///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
static void DoPolyTextureClip(VertexList &theList)
{
	VertexList l2;

	float left = 0;
	float right = 1;
	float top = 0;
	float bottom = 1;

	VertexList *in = &theList, *out = &l2;
	PointClipper<std::less<float> > aLessClipper;
	PointClipper<std::greater_equal<float> > aGreaterClipper;

	aLessClipper.ClipPoints(3,left,*in,*out); std::swap(in,out); out->clear();
	aLessClipper.ClipPoints(4,top,*in,*out); std::swap(in,out); out->clear();
	aGreaterClipper.ClipPoints(3,right,*in,*out); std::swap(in,out); out->clear();
	aGreaterClipper.ClipPoints(4,bottom,*in,*out);
	theList = *out;
}


void TextureData::BltTransformed(const SexyMatrix3 &theTrans, const Rect& theSrcRect, const Color& theColor, const Rect *theClipRect, float theX, float theY, bool center)
{
	int srcLeft = theSrcRect.mX;
	int srcTop = theSrcRect.mY;
	int srcRight = srcLeft + theSrcRect.mWidth;
	int srcBottom = srcTop + theSrcRect.mHeight;
	int srcX, srcY;
	float dstX, dstY;
	int aWidth;
	int aHeight;
	float u1,v1,u2,v2;
	float startx = 0, starty = 0;
	float pixelcorrect = 0.5f;

	if (center)
	{
		startx = -theSrcRect.mWidth/2.0f;
		starty = -theSrcRect.mHeight/2.0f;
		pixelcorrect = 0.0f;
	}

	srcY = srcTop;
	dstY = starty;

	uint32_t aColor = (theColor.mRed << 0) | (theColor.mGreen << 8) | (theColor.mBlue << 16) | (theColor.mAlpha << 24);

	if ((srcLeft >= srcRight) || (srcTop >= srcBottom))
		return;

	while(srcY < srcBottom)
	{
		srcX = srcLeft;
		dstX = startx;
		while(srcX < srcRight)
		{
			aWidth = srcRight-srcX;
			aHeight = srcBottom-srcY;
			GXTexObj* aTexture = GetTexture(srcX, srcY, aWidth, aHeight, u1, v1, u2, v2);

			float x = dstX; // - 0.5f;
			float y = dstY; // - 0.5f;

			SexyVector2 p[4] = { SexyVector2(x, y), SexyVector2(x,y+aHeight), SexyVector2(x+aWidth, y) , SexyVector2(x+aWidth, y+aHeight) };
			SexyVector2 tp[4];

			int i;
			for (i=0; i<4; i++)
			{
				tp[i] = theTrans*p[i];
				tp[i].x -= pixelcorrect - theX;
				tp[i].y -= pixelcorrect - theY;
			}

			bool clipped = false;
			if (theClipRect != NULL)
			{
				int left = theClipRect->mX;
				int right = left + theClipRect->mWidth;
				int top = theClipRect->mY;
				int bottom = top + theClipRect->mHeight;
				for (i=0; i<4; i++)
				{
					if (tp[i].x<left || tp[i].x>=right || tp[i].y<top || tp[i].y>=bottom)
					{
						clipped = true;
						break;
					}
				}
			}

			GLVertex aVertex[4] = {
				{ {tp[0].x},{tp[0].y},{0},{aColor},{u1},{v1} },
				{ {tp[1].x},{tp[1].y},{0},{aColor},{u1},{v2} },
				{ {tp[3].x},{tp[3].y},{0},{aColor},{u2},{v2} },
				{ {tp[2].x},{tp[2].y},{0},{aColor},{u2},{v1} }
			};

			SetTexture(aTexture);

			if (!clipped)
			{
				GxDrawVerts(GX_QUADS, aVertex, 4, true);
			}
			else
			{
				VertexList aList;
				aList.push_back(aVertex[0]);
				aList.push_back(aVertex[1]);
				aList.push_back(aVertex[2]);
				aList.push_back(aVertex[3]);

				DrawPolyClipped(theClipRect, aList, true);
			}

			srcX += aWidth;
			dstX += aWidth;
		}

		srcY += aHeight;
		dstY += aHeight;
	}
}

void TextureData::BltTriangles(const TriVertex theVertices[][3], int theNumTriangles, unsigned int theColor, float tx, float ty)
{
	if ((mMaxTotalU <= 1.0) && (mMaxTotalV <= 1.0))
	{
		SetTexture(&mTextures[0].mTexture);

		VertexList aList;
		for (int aTriangleNum = 0; aTriangleNum < theNumTriangles; aTriangleNum++)
		{
			const TriVertex* aTriVerts = theVertices[aTriangleNum];
			for (int i = 0; i < 3; i++)
			{
				GLVertex v = {
					{aTriVerts[i].x + tx},{aTriVerts[i].y + ty},{0},
					{GetColorFromTriVertex(aTriVerts[i],theColor)},
					{aTriVerts[i].u*mMaxTotalU},{aTriVerts[i].v*mMaxTotalV}
				};
				aList.push_back(v);
			}
		}
		GxDrawVerts(GX_TRIANGLES, aList, true);
	}
	else
	{
		for (int aTriangleNum = 0; aTriangleNum < theNumTriangles; aTriangleNum++)
		{
			TriVertex* aTriVerts = (TriVertex*) theVertices[aTriangleNum];

			GLVertex aVertex[3] = {
				{ {aTriVerts[0].x + tx},{aTriVerts[0].y + ty},	{0},{GetColorFromTriVertex(aTriVerts[0],theColor)},	{aTriVerts[0].u*mMaxTotalU},{aTriVerts[0].v*mMaxTotalV} },
				{ {aTriVerts[1].x + tx},{aTriVerts[1].y + ty},	{0},{GetColorFromTriVertex(aTriVerts[1],theColor)},	{aTriVerts[1].u*mMaxTotalU},{aTriVerts[1].v*mMaxTotalV} },
				{ {aTriVerts[2].x + tx},{aTriVerts[2].y + ty},	{0},{GetColorFromTriVertex(aTriVerts[2],theColor)},	{aTriVerts[2].u*mMaxTotalU},{aTriVerts[2].v*mMaxTotalV} }
			};

			float aMinU = mMaxTotalU, aMinV = mMaxTotalV;
			float aMaxU = 0, aMaxV = 0;

			int i,j,k;
			for (i=0; i<3; i++)
			{
				if(aVertex[i].tu < aMinU)
					aMinU = aVertex[i].tu;

				if(aVertex[i].tv < aMinV)
					aMinV = aVertex[i].tv;

				if(aVertex[i].tu > aMaxU)
					aMaxU = aVertex[i].tu;

				if(aVertex[i].tv > aMaxV)
					aMaxV = aVertex[i].tv;
			}

			VertexList aMasterList;
			aMasterList.push_back(aVertex[0]);
			aMasterList.push_back(aVertex[1]);
			aMasterList.push_back(aVertex[2]);


			int aLeft = floorf(aMinU);
			int aTop = floorf(aMinV);
			int aRight = ceilf(aMaxU);
			int aBottom = ceilf(aMaxV);
			if (aLeft < 0)
				aLeft = 0;
			if (aTop < 0)
				aTop = 0;
			if (aRight > mTexVecWidth)
				aRight = mTexVecWidth;
			if (aBottom > mTexVecHeight)
				aBottom = mTexVecHeight;

			TextureDataPiece &aStandardPiece = mTextures[0];
			for (i=aTop; i<aBottom; i++)
			{
				for (j=aLeft; j<aRight; j++)
				{
					TextureDataPiece &aPiece = mTextures[i*mTexVecWidth + j];

					VertexList aList = aMasterList;
					for(k=0; k<3; k++)
					{
						aList[k].tu -= j;
						aList[k].tv -= i;
						if (i==mTexVecHeight-1)
							aList[k].tv *= (float)aStandardPiece.mHeight / aPiece.mHeight;
						if (j==mTexVecWidth-1)
							aList[k].tu *= (float)aStandardPiece.mWidth / aPiece.mWidth;
					}

					DoPolyTextureClip(aList);
					if (aList.size() >= 3)
					{
						SetTexture(&aPiece.mTexture);
						GxDrawVerts(GX_TRIANGLEFAN, aList, true);
					}
				}
			}
		}
	}
}


GLInterface::GLInterface(SexyAppBase* theApp)
{
	mApp = theApp;
	mWidth = mApp->mWidth;
	mHeight = mApp->mHeight;
	mDisplayWidth = mWidth;
	mDisplayHeight = mHeight;

	mPresentationRect = Rect( 0, 0, mWidth, mHeight );

	mRefreshRate = 60;
	mMillisecondsPerFrame = 1000/mRefreshRate;

	mScreenImage = 0;

	mNextCursorX = 0;
	mNextCursorY = 0;
	mCursorX = 0;
	mCursorY = 0;
}

GLInterface::~GLInterface()
{
	Flush();

	ImageSet::iterator anItr;
	for(anItr = mImageSet.begin(); anItr != mImageSet.end(); ++anItr)
	{
		MemoryImage *anImage = *anItr;
		delete (TextureData*)anImage->mD3DData;
		anImage->mD3DData = NULL;
	}
}

void GLInterface::SetDrawMode(int theDrawMode)
{
	if (theDrawMode == Graphics::DRAWMODE_NORMAL)
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	else // Additive
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
}

void GLInterface::AddGLImage(GLImage* theGLImage)
{
	AutoCrit anAutoCrit(mCritSect);

	mGLImageSet.insert(theGLImage);
}

void GLInterface::RemoveGLImage(GLImage* theGLImage)
{
	AutoCrit anAutoCrit(mCritSect);

	GLImageSet::iterator anItr = mGLImageSet.find(theGLImage);
	if (anItr != mGLImageSet.end())
		mGLImageSet.erase(anItr);
}

void GLInterface::Remove3DData(MemoryImage* theImage)
{
	if (theImage->mD3DData != NULL)
	{
		delete (TextureData*)theImage->mD3DData;
		theImage->mD3DData = NULL;

		AutoCrit aCrit(mCritSect); // Make images thread safe
		mImageSet.erase(theImage);
	}
}

GLImage* GLInterface::GetScreenImage()
{
	return mScreenImage;
}

void GLInterface::UpdateViewport()
{
	// Restrict to 4:3, same idea as the PC/3DS backends
	int width = gRMode->fbWidth;
	int height = gRMode->efbHeight;

	int viewport_x = 0;
	int viewport_y = 0;
	int viewport_width = width;
	int viewport_height = height;
	if (width * 3 > height * 4)
	{
		viewport_width = height * 4 / 3;
		viewport_x = (width - viewport_width) / 2;
	}
	else if (width * 3 < height * 4)
	{
		viewport_height = width * 3 / 4;
		viewport_y = (height - viewport_height) / 2;
	}

	GX_SetViewport(viewport_x, viewport_y, viewport_width, viewport_height, 0, 1);
	mPresentationRect = Rect( viewport_x, viewport_y, viewport_width, viewport_height );
}

int GLInterface::Init(bool IsWindowed)
{
	static bool inited = false;
	if (!inited)
	{
		inited = true;

		VIDEO_Init();
		gRMode = VIDEO_GetPreferredMode(NULL);

		gXfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(gRMode));
		gXfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(gRMode));
		gFbIndex = 0;

		VIDEO_Configure(gRMode);
		VIDEO_SetNextFramebuffer(gXfb[gFbIndex]);
		VIDEO_SetBlack(FALSE);
		VIDEO_Flush();
		VIDEO_WaitVSync();
		if (gRMode->viTVMode & VI_NON_INTERLACE)
			VIDEO_WaitVSync();

		gGpFifo = memalign(32, DEFAULT_FIFO_SIZE);
		memset(gGpFifo, 0, DEFAULT_FIFO_SIZE);
		GX_Init(gGpFifo, DEFAULT_FIFO_SIZE);

		GXColor aBackground = {0, 0, 0, 0xff};
		GX_SetCopyClear(aBackground, GX_MAX_Z24);

		GX_SetViewport(0, 0, gRMode->fbWidth, gRMode->efbHeight, 0, 1);
		GX_SetDispCopyYScale((f32)gRMode->xfbHeight / (f32)gRMode->efbHeight);
		GX_SetScissor(0, 0, gRMode->fbWidth, gRMode->efbHeight);
		GX_SetDispCopySrc(0, 0, gRMode->fbWidth, gRMode->efbHeight);
		GX_SetDispCopyDst(gRMode->fbWidth, gRMode->xfbHeight);
		GX_SetCopyFilter(gRMode->aa, gRMode->sample_pattern, GX_TRUE, gRMode->vfilter);
		GX_SetFieldMode(gRMode->field_rendering, ((gRMode->viHeight == 2 * gRMode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
		GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
		GX_SetCullMode(GX_CULL_NONE);
		GX_SetDispCopyGamma(GX_GM_1_0);

		GX_ClearVtxDesc();
		GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
		GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
		GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
		GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
		GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);

		// Just pass the raster (per-vertex) color through - no lighting
		GX_SetNumChans(1);
		GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);

		GX_SetNumTevStages(1);
		GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_TRUE);
		GX_SetColorUpdate(GX_TRUE);
		GX_SetAlphaUpdate(GX_TRUE);
	}

	guOrtho(gProjection, 0, mHeight-1, 0, mWidth-1, 0, 300);
	GX_LoadProjectionMtx(gProjection, GX_ORTHOGRAPHIC);

	// Use untextured draw mode by default
	SetTexture(NULL, true);

	gTextureSizeMustBePow2 = false;
	gMinTextureWidth = 8;
	gMinTextureHeight = 8;
	gMaxTextureWidth = 512;
	gMaxTextureHeight = 512;
	gSupportedPixelFormats = PixelFormat_A8R8G8B8 | PixelFormat_A4R4G4B4 | PixelFormat_R5G6B5;
	gLinearFilter = false;

	mRGBBits = 32;

	mRedBits = 8;
	mGreenBits = 8;
	mBlueBits = 8;

	mRedShift = 0;
	mGreenShift = 8;
	mBlueShift = 16;

	mRedMask = (0xFFU << mRedShift);
	mGreenMask = (0xFFU << mGreenShift);
	mBlueMask = (0xFFU << mBlueShift);

	// mDisplayWidth/Height reflect the real screen resolution (used by
	// UpdateViewport/Input's WPAD_SetVRes), separate from the logical,
	// possibly-downscaled mWidth/mHeight game resolution
	mDisplayWidth = gRMode->fbWidth;
	mDisplayHeight = gRMode->xfbHeight;

	SetVideoOnlyDraw(false);

	return 1;
}

bool GLInterface::Redraw(Rect* theClipRect)
{
	Flush();
	return true;
}

void GLInterface::SetVideoOnlyDraw(bool videoOnly)
{
	if (mScreenImage) delete mScreenImage;
	mScreenImage = new GLImage(this);
	mScreenImage->mWidth = mWidth;
	mScreenImage->mHeight = mHeight;
	mScreenImage->SetImageMode(false, false);
}

void GLInterface::SetCursorPos(int theCursorX, int theCursorY)
{
	mNextCursorX = theCursorX;
	mNextCursorY = theCursorY;
}

bool GLInterface::PreDraw()
{
	gLinearFilter = false;
	return true;
}

void GLInterface::Flush()
{
	GX_SetColorUpdate(GX_TRUE);
	GX_CopyDisp(gXfb[gFbIndex], GX_TRUE);
	GX_DrawDone(); // blocks until the GP has finished the EFB->XFB copy above

	VIDEO_SetNextFramebuffer(gXfb[gFbIndex]);
	VIDEO_Flush();
	VIDEO_WaitVSync();

	gFbIndex ^= 1;
}

bool GLInterface::CreateImageTexture(MemoryImage *theImage)
{
	bool wantPurge = false;

	if(theImage->mD3DData==NULL)
	{
		theImage->mD3DData = new TextureData();

		// The actual purging was deferred
		wantPurge = theImage->mPurgeBits;

		AutoCrit aCrit(mCritSect); // Make images thread safe
		mImageSet.insert(theImage);
	}

	TextureData *aData = (TextureData*)theImage->mD3DData;
	aData->CheckCreateTextures(theImage);

	if (wantPurge)
		theImage->PurgeBits();

	return aData->mPixelFormat != PixelFormat_Unknown;
}

bool GLInterface::RecoverBits(MemoryImage* theImage)
{
	// GX textures are CPU-writable buffers we own directly (no GPU-side-only
	// storage the way D3D surfaces were), so there's nothing to recover -
	// the source MemoryImage bits are the ones we tiled from in the first place.
	if (theImage->mD3DData == NULL)
		return false;

	TextureData* aData = (TextureData*) theImage->mD3DData;
	return aData->mBitsChangedCount == theImage->mBitsChangedCount;
}

void GLInterface::PushTransform(const SexyMatrix3 &theTransform, bool concatenate)
{
	if (mTransformStack.empty() || !concatenate)
		mTransformStack.push_back(theTransform);
	else
	{
		SexyMatrix3 &aTrans = mTransformStack.back();
		mTransformStack.push_back(theTransform*aTrans);
	}
}

void GLInterface::PopTransform()
{
	if (!mTransformStack.empty())
		mTransformStack.pop_back();
}

void GLInterface::Blt(Image* theImage, float theX, float theY, const Rect& theSrcRect, const Color& theColor, int theDrawMode, bool linearFilter)
{
	if (!mTransformStack.empty())
	{
		BltClipF(theImage,theX,theY,theSrcRect,NULL,theColor,theDrawMode);
		return;
	}

	if (!PreDraw())
		return;

	MemoryImage* aSrcMemoryImage = (MemoryImage*) theImage;

	if (!CreateImageTexture(aSrcMemoryImage))
		return;

	SetDrawMode(theDrawMode);

	TextureData *aData = (TextureData*)aSrcMemoryImage->mD3DData;

	SetLinearFilter(linearFilter);
	aData->Blt(theX,theY,theSrcRect,theColor);
}

void GLInterface::BltClipF(Image* theImage, float theX, float theY, const Rect& theSrcRect, const Rect *theClipRect, const Color& theColor, int theDrawMode)
{
	SexyTransform2D aTransform;
	aTransform.Translate(theX, theY);

	BltTransformed(theImage,theClipRect,theColor,theDrawMode,theSrcRect,aTransform,true);
}

void GLInterface::BltMirror(Image* theImage, float theX, float theY, const Rect& theSrcRect, const Color& theColor, int theDrawMode, bool linearFilter)
{
	SexyTransform2D aTransform;

	aTransform.Translate(-theSrcRect.mWidth,0);
	aTransform.Scale(-1, 1);
	aTransform.Translate(theX, theY);

	BltTransformed(theImage,NULL,theColor,theDrawMode,theSrcRect,aTransform,linearFilter);
}

void GLInterface::StretchBlt(Image* theImage,  const Rect& theDestRect, const Rect& theSrcRect, const Rect* theClipRect, const Color &theColor, int theDrawMode, bool fastStretch, bool mirror)
{
	float xScale = (float)theDestRect.mWidth / theSrcRect.mWidth;
	float yScale = (float)theDestRect.mHeight / theSrcRect.mHeight;

	SexyTransform2D aTransform;
	if (mirror)
	{
		aTransform.Translate(-theSrcRect.mWidth,0);
		aTransform.Scale(-xScale, yScale);
	}
	else
		aTransform.Scale(xScale, yScale);

	aTransform.Translate(theDestRect.mX, theDestRect.mY);
	BltTransformed(theImage,theClipRect,theColor,theDrawMode,theSrcRect,aTransform,!fastStretch);
}

void GLInterface::BltRotated(Image* theImage, float theX, float theY, const Rect* theClipRect, const Color& theColor, int theDrawMode, double theRot, float theRotCenterX, float theRotCenterY, const Rect& theSrcRect)
{
	SexyTransform2D aTransform;

	aTransform.Translate(-theRotCenterX, -theRotCenterY);
	aTransform.RotateRad(theRot);
	aTransform.Translate(theX+theRotCenterX,theY+theRotCenterY);

	BltTransformed(theImage,theClipRect,theColor,theDrawMode,theSrcRect,aTransform,true);
}

void GLInterface::BltTransformed(Image* theImage, const Rect* theClipRect, const Color& theColor, int theDrawMode, const Rect &theSrcRect, const SexyMatrix3 &theTransform, bool linearFilter, float theX, float theY, bool center)
{
	if (!PreDraw())
		return;

	MemoryImage* aSrcMemoryImage = (MemoryImage*) theImage;

	if (!CreateImageTexture(aSrcMemoryImage))
		return;

	SetDrawMode(theDrawMode);

	TextureData *aData = (TextureData*)aSrcMemoryImage->mD3DData;

	if (!mTransformStack.empty())
	{
		SetLinearFilter(true); // force linear filtering in the case of a global transform
		if (theX!=0 || theY!=0)
		{
			SexyTransform2D aTransform;
			if (center)
				aTransform.Translate(-theSrcRect.mWidth/2.0f,-theSrcRect.mHeight/2.0f);

			aTransform = theTransform * aTransform;
			aTransform.Translate(theX,theY);
			aTransform = mTransformStack.back() * aTransform;

			aData->BltTransformed(aTransform, theSrcRect, theColor, theClipRect);
		}
		else
		{
			SexyTransform2D aTransform = mTransformStack.back()*theTransform;
			aData->BltTransformed(aTransform, theSrcRect, theColor, theClipRect, theX, theY, center);
		}
	}
	else
	{
		SetLinearFilter(linearFilter);
		aData->BltTransformed(theTransform, theSrcRect, theColor, theClipRect, theX, theY, center);
	}
}

void GLInterface::DrawLine(double theStartX, double theStartY, double theEndX, double theEndY, const Color& theColor, int theDrawMode)
{
	if (!PreDraw())
		return;

	SetDrawMode(theDrawMode);

	float x1, y1, x2, y2;

	if (!mTransformStack.empty())
	{
		SexyVector2 p1(theStartX,theStartY);
		SexyVector2 p2(theEndX,theEndY);
		p1 = mTransformStack.back()*p1;
		p2 = mTransformStack.back()*p2;

		x1 = p1.x;
		y1 = p1.y;
		x2 = p2.x;
		y2 = p2.y;
	}
	else
	{
		x1 = theStartX;
		y1 = theStartY;
		x2 = theEndX;
		y2 = theEndY;
	}

	SetTexture(NULL);

	GLVertex aVertex[2] = {
		{ {x1},{y1},{0},{theColor.ToInt()},{0},{0} },
		{ {x2},{y2},{0},{theColor.ToInt()},{0},{0} },
	};

	GxDrawVerts(GX_LINES, aVertex, 2, false);
}

void GLInterface::FillRect(const Rect& theRect, const Color& theColor, int theDrawMode)
{
	if (!PreDraw())
		return;

	SetDrawMode(theDrawMode);

	float x = theRect.mX - 0.5f;
	float y = theRect.mY - 0.5f;
	float aWidth = theRect.mWidth;
	float aHeight = theRect.mHeight;

	GLVertex aVertex[4] = {
		{ {x},        {y},         {0},{theColor.ToInt()},{0},{0} },
		{ {x},        {y+aHeight}, {0},{theColor.ToInt()},{0},{0} },
		{ {x+aWidth}, {y+aHeight}, {0},{theColor.ToInt()},{0},{0} },
		{ {x+aWidth}, {y},         {0},{theColor.ToInt()},{0},{0} }
	};

	if (!mTransformStack.empty())
	{
		SexyVector2 p[4] = { SexyVector2(x, y), SexyVector2(x,y+aHeight), SexyVector2(x+aWidth, y+aHeight), SexyVector2(x+aWidth, y) };

		int i;
		for (i=0; i<4; i++)
		{
			p[i] = mTransformStack.back()*p[i];
			p[i].x -= 0.5f;
			p[i].y -= 0.5f;
			aVertex[i].sx = p[i].x;
			aVertex[i].sy = p[i].y;
		}
	}

	SetTexture(NULL);
	GxDrawVerts(GX_QUADS, aVertex, 4, false);
}

void GLInterface::DrawTriangle(const TriVertex &p1, const TriVertex &p2, const TriVertex &p3, const Color &theColor, int theDrawMode)
{
	if (!PreDraw())
		return;

	SetDrawMode(theDrawMode);

	unsigned int aColor = (theColor.mRed << 0) | (theColor.mGreen << 8) | (theColor.mBlue << 16) | (theColor.mAlpha << 24);
	unsigned int col1 = GetColorFromTriVertex(p1, aColor);
	unsigned int col2 = GetColorFromTriVertex(p2, aColor);
	unsigned int col3 = GetColorFromTriVertex(p3, aColor);

	SetTexture(NULL);

	GLVertex aVertex[3] = {
		{ {p1.x}, {p1.y}, {0}, {col1}, {0},{0} },
		{ {p2.x}, {p2.y}, {0}, {col2}, {0},{0} },
		{ {p3.x}, {p3.y}, {0}, {col3}, {0},{0} },
	};

	GxDrawVerts(GX_TRIANGLES, aVertex, 3, false);
}

void GLInterface::DrawTriangleTex(const TriVertex &p1, const TriVertex &p2, const TriVertex &p3, const Color &theColor, int theDrawMode, Image *theTexture, bool blend)
{
	TriVertex aVertices[1][3] = {{p1, p2, p3}};
	DrawTrianglesTex(aVertices,1,theColor,theDrawMode,theTexture,0,0,blend);
}

void GLInterface::DrawTrianglesTex(const TriVertex theVertices[][3], int theNumTriangles, const Color &theColor, int theDrawMode, Image *theTexture, float tx, float ty, bool blend)
{
	if (!PreDraw()) return;

	MemoryImage* aSrcMemoryImage = (MemoryImage*)theTexture;

	if (!CreateImageTexture(aSrcMemoryImage))
		return;

	SetDrawMode(theDrawMode);

	TextureData *aData = (TextureData*)aSrcMemoryImage->mD3DData;

	SetLinearFilter(blend);

	unsigned int aColor = (theColor.mRed << 0) | (theColor.mGreen << 8) | (theColor.mBlue << 16) | (theColor.mAlpha << 24);
	aData->BltTriangles(theVertices, theNumTriangles, aColor, tx, ty);
}

void GLInterface::DrawTrianglesTexStrip(const TriVertex theVertices[], int theNumTriangles, const Color &theColor, int theDrawMode, Image *theTexture, float tx, float ty, bool blend)
{
	TriVertex aList[100][3];
	int aTriNum = 0;
	while (aTriNum < theNumTriangles)
	{
		int aMaxTriangles = std::min(100,theNumTriangles - aTriNum);
		for (int i=0; i<aMaxTriangles; i++)
		{
			aList[i][0] = theVertices[aTriNum];
			aList[i][1] = theVertices[aTriNum+1];
			aList[i][2] = theVertices[aTriNum+2];
			aTriNum++;
		}
		DrawTrianglesTex(aList,aMaxTriangles,theColor,theDrawMode,theTexture, tx, ty, blend);
	}
}

void GLInterface::FillPoly(const Point theVertices[], int theNumVertices, const Rect *theClipRect, const Color &theColor, int theDrawMode, int tx, int ty)
{
	if (theNumVertices<3)
		return;

	if (!PreDraw())
		return;

	SetDrawMode(theDrawMode);
	unsigned int aColor = (theColor.mRed << 0) | (theColor.mGreen << 8) | (theColor.mBlue << 16) | (theColor.mAlpha << 24);

	SetTexture(NULL);

	VertexList aList;
	for (int i=0; i<theNumVertices; i++)
	{
		GLVertex vert = { {theVertices[i].mX + (float)tx}, {theVertices[i].mY + (float)ty}, {0}, {aColor}, {0}, {0} };
		if (!mTransformStack.empty())
		{
			SexyVector2 v(vert.sx,vert.sy);
			v = mTransformStack.back()*v;
			vert.sx = v.x;
			vert.sy = v.y;
		}

		aList.push_back(vert);
	}

	if (theClipRect != NULL)
		DrawPolyClipped(theClipRect, aList, false);
	else
		GxDrawVerts(GX_TRIANGLEFAN, aList, false);
}
