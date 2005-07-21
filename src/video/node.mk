# $Id$

include build/node-start.mk

SUBDIRS:= \
	v9990

SRC_HDR:= \
	VDP VDPCmdEngine VDPVRAM SpriteChecker \
	VDPSettings \
	VideoSourceSetting \
	Renderer RendererFactory RenderSettings PixelRenderer \
	SDLVideoSystem SDLRasterizer SDLSnow \
	SDLUtil \
	DummyRenderer \
	XRenderer \
	BitmapConverter CharacterConverter \
	Scaler SimpleScaler SaI2xScaler Scale2xScaler \
	HQ2xScaler HQ2xLiteScaler LowScaler \
	ScreenShotSaver \
	VideoSystem Display VideoLayer \
	Layer \
	DummyVideoSystem \
	IconLayer \
	SDLImage \
	MemoryOps \
	RawFrame PostProcessor

HDR_ONLY:= \
	DirtyChecker \
	DisplayMode \
	VRAMObserver \
	SpriteConverter \
	Blender \
	Rasterizer \
	Icon

SRC_HDR_$(COMPONENT_GL)+= \
	SDLGLVideoSystem GLRasterizer GLSnow GLUtil GLImage

include build/node-end.mk

