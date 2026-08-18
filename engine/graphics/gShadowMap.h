/*
 * Copyright (C) 2016 Nitra Games Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GRAPHICS_GSHADOWMAP_H_
#define GRAPHICS_GSHADOWMAP_H_

#include "gRenderObject.h"
#include "gFbo.h"
#include "gLight.h"
#include "gCamera.h"


class gShadowMap : public gRenderObject {
public:
	gShadowMap();
	virtual ~gShadowMap();

	void allocate(gLight* light, gCamera* camera, int width = 4096, int height = 4096);
	bool isAllocated() const;
	int getWidth() const;
	int getHeight() const;

	void update();

	void setLight(gLight* light);
	void setCamera(gCamera* camera);
	gLight* getLight() const;
	gCamera* getCamera() const;

	void activate();
	void deactivate();
	bool isActivated() const;

	void enable();
	void disable();
	bool isEnabled() const;

	void setLightProjection(glm::mat4 lightProjection);
	void setLightProjection(float leftx, float rightx, float fronty, float backy, float nearz, float farz);
	void setLightView(glm::mat4 lightView);
	glm::mat4 getLightProjection() const;
	glm::mat4 getLightView() const;
	glm::mat4 getLightMatrix() const;

	// Camera-fitted shadow volume. This is off by default and nothing about the class
	// changes until it is switched on, because the shadow box an application passed to
	// setLightProjection() is a number it tuned by looking at its own scene, and the
	// engine has no business overruling it behind its back. Once a game opts in, this
	// class takes ownership of both the light projection and the light view and rebuilds
	// them at the start of every shadow pass so that the volume follows the camera; any
	// setLightProjection() / setLightView() the application still makes will therefore be
	// overwritten on the next frame. In other words: opting in is the application saying
	// "you drive", and disableCameraFit() hands the wheel back, leaving whatever matrices
	// were last set in place.
	//
	// shadowDistance is how far down the camera's view direction the shadow map is asked
	// to reach, in world units. It is the single most important knob here: the whole
	// point of fitting is that the fixed texel budget is spent on a small slab of world
	// instead of the whole map, so a smaller distance buys sharper shadows and a larger
	// one buys reach. Passing zero (or anything negative) means "as far as the camera can
	// see" and falls back to the camera's far clip, which is usually far too generous to
	// be worth it but is at least never wrong.
	void enableCameraFit(float shadowDistance = 0.0f);
	void disableCameraFit();
	bool isCameraFitEnabled() const;
	void setShadowDistance(float distance);
	float getShadowDistance() const;

	// How far the fitted volume's near plane is pushed back towards the light, in world
	// units. A box fitted to exactly what the camera can see would clip away everything
	// between the light and that box, and those are precisely the objects whose shadows
	// fall into view - a wall just off screen, the roof above the player. This value is
	// how much of that off-screen space is kept, so it wants to be at least as tall as
	// the tallest caster that can reach into the view.
	void setShadowCasterExtrusion(float distance);
	float getShadowCasterExtrusion() const;

	// World units covered by one texel of the single fitted volume. Only meaningful while
	// camera fitting is on; it is what a shader would scale a depth bias or a normal offset
	// by if it wanted them to mean a fixed distance rather than a fixed fraction of a
	// frustum whose size now changes with the camera.
	float getTexelWorldSize() const;

	// --- Cascaded shadow maps -------------------------------------------------------
	//
	// A single fitted volume already spends its texels on what the camera can see, but it
	// spends them uniformly, and the eye's demand for detail is anything but uniform: a
	// shadow ten units away covers a hundred times the screen area of the same shadow a
	// hundred units away and wants a hundred times the density. Cascades cut the shadow
	// distance into ranges and give each range its own fitted, snapped volume, so the near
	// range gets a map to itself.
	//
	// What this class computes is the whole camera-side half of that: the split distances,
	// one fitted and snapped matrix per cascade, the world size of a cascade's texel, and
	// where each cascade lives in the atlas. What it deliberately does NOT do is change
	// what the backends are handed today. getLightMatrix() goes on being a single volume
	// covering the entire shadow distance, computed independently of these, because:
	//
	//  - the OpenGL shaders take one light matrix and one sampler2D and must not be
	//    touched, so cascades can never be transparent to that backend. Fitted-single is
	//    what OpenGL gets, permanently and by design.
	//  - the Vulkan shaders need a cascade array in their scene uniform block and a
	//    selection rule in their fragment stage before any of this is readable, and a
	//    uniform block that grew on one side only would break every Vulkan build.
	//
	// So the cascade data sits here, computed and correct, and a backend consumes it when
	// its shaders are ready. Until then setCascadeNum() costs one extra fit per frame and
	// changes nothing anybody can see.
	static const int MAXCASCADENUM = 4;

	// 1 to 4. One means the cascade data simply mirrors the single fitted volume.
	void setCascadeNum(int num);
	int getCascadeNum() const;

	// Where the splits land, between a uniform division of the range (0) and a logarithmic
	// one (1). Uniform wastes the sharp cascades on distance the eye does not care about;
	// logarithmic is what perspective actually asks for but crams the first cascade into
	// the few centimetres in front of the near clip plane. Practical values sit in between.
	void setCascadeSplitLambda(float lambda);
	float getCascadeSplitLambda() const;

	// The fraction of each cascade's range that overlaps into the previous one, which is
	// the band a fragment shader cross-fades over. Zero gives a hard line across the ground
	// wherever the cascade changes, because the texel size and with it the exact shape of
	// every filtered edge changes in a single pixel.
	void setCascadeBlendRatio(float ratio);
	float getCascadeBlendRatio() const;

	// lightProjection * lightView for one cascade, ready to be uploaded as-is.
	const glm::mat4& getCascadeMatrix(int index) const;
	// The view-space distance at which this cascade stops and the next begins. This is the
	// number the fragment shader compares a fragment's view depth against to choose one.
	float getCascadeSplitDistance(int index) const;
	// World units per texel in this cascade, for scaling bias and normal offset per
	// cascade. Without this the bias tuned for the sharpest cascade acnes in the coarsest.
	float getCascadeTexelWorldSize(int index) const;
	// Where this cascade lives inside the shadow map, as x/y offset and z/w scale in
	// normalised texture coordinates. The cascades share one image rather than taking a
	// texture array, because an atlas needs no change to the descriptor set layout, no
	// second sampler and no second render pass - only a viewport per cascade in the depth
	// pass and two multiply-adds in the fragment shader.
	glm::vec4 getCascadeAtlasRect(int index) const;
	int getCascadeAtlasColumns() const;
	int getCascadeAtlasRows() const;
	int getCascadeTileWidth() const;
	int getCascadeTileHeight() const;

	gFbo& getDepthFbo();


private:
	// Fits one orthographic box to the slice of the camera frustum between two distances,
	// in the space of an already oriented light, and snaps it to the texel grid of a tile
	// of the given size. Everything else here is bookkeeping around how often this is
	// called and with what.
	void fitVolume(const glm::mat4& lightrotation, float neardistance, float fardistance,
			int tilewidth, int tileheight, glm::mat4& outprojection, float& outtexelworldsize);
	// Recomputes lightview, lightprojection and the cascade data from the camera's frustum.
	// Does nothing unless camera fitting is on and both a camera and a light are attached.
	void fitToCamera();

	bool isallocated, isactivated, isenabled;
	gLight* light;
	gCamera* camera;
	glm::mat4 lightprojection, lightview, lightmatrix;
	glm::vec3 lightposition;
	gFbo depthfbo;
	int width, height;
	int shadowmaptextureslot;
	bool updateshadows;
	bool camerafitenabled;
	float shadowdistance;
	float shadowcasterextrusion;
	float legacytexelworldsize;
	int cascadenum;
	float cascadesplitlambda;
	float cascadeblendratio;
	glm::mat4 cascadematrix[MAXCASCADENUM];
	float cascadesplitdistance[MAXCASCADENUM];
	float cascadetexelworldsize[MAXCASCADENUM];
	// Width, in shadow map texels, of the unwritten frame kept around the fitted volume.
	// See the long note next to the scissor call in gShadowMap.cpp: it is what keeps the
	// OpenGL path's clamp-to-edge depth texture from smearing the border texels' depths
	// across everything that lies outside the fit.
	int fitborder;
};

#endif /* GRAPHICS_GSHADOWMAP_H_ */
