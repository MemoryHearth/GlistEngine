/*
 * gShadowMap.cpp
 *
 *  Created on: Mar 21, 2021
 *      Author: noyan
 */

#include "gShadowMap.h"
#include "gShader.h"
#include "gTracy.h"

#include <cmath>

gShadowMap::gShadowMap() {
	isallocated = false;
	isactivated = false;
	isenabled = false;
	camera = nullptr;
	light = nullptr;
	lightprojection = glm::mat4(1.0f);
	lightview = glm::mat4(1.0f);
	lightmatrix = lightprojection * lightview;
	width = 0;
	height = 0;
	shadowmaptextureslot = 9;
	updateshadows = false;
	camerafitenabled = false;
	shadowdistance = 0.0f;
	// Twelve units of extrusion is enough for a person, a doorway or a low wall standing
	// just off screen to still cast into the view, and short enough that the depth range
	// stays tight. A game with tall casters - a cliff, a skyscraper - should raise it.
	shadowcasterextrusion = 12.0f;
	fitborder = 2;
	legacytexelworldsize = 0.0f;
	cascadenum = 1;
	// Six tenths of the way from a uniform split towards a logarithmic one. Pure log is
	// what perspective wants and is unusable with a near clip of a hundredth of a unit;
	// pure uniform gives the near cascade almost none of the range it deserves.
	cascadesplitlambda = 0.6f;
	cascadeblendratio = 0.1f;
	for (int i = 0; i < MAXCASCADENUM; i++) {
		cascadematrix[i] = glm::mat4(1.0f);
		cascadesplitdistance[i] = 0.0f;
		cascadetexelworldsize[i] = 0.0f;
	}
}

// The eight world space corners of the slice of the camera frustum that the shadow map is
// asked to cover: the same perspective frustum gCamera::begin() builds its culling planes
// from, but cut off at nearDistance and farDistance instead of the camera's own clip
// planes. It is written out by hand rather than by inverse-projecting the NDC cube because
// the two backends disagree about what the NDC depth range even is - OpenGL wants -1..1
// and Vulkan wants 0..1 - and this way the corners come out identical on both, which is
// the whole reason one gShadowMap can drive both backends.
static void gShadowMapComputeFrustumCorners(const glm::mat4& cameralookmatrix, float fovdegrees,
		float aspect, float neardistance, float fardistance, glm::vec3* outcorners) {
	// The look matrix is the camera's local-to-world transform, so its first three columns
	// are the camera's basis vectors in world space and its fourth is the eye position. It
	// may carry a scale (gCamera::setScale exists and applies to it), which would make the
	// basis vectors longer than one unit and quietly inflate every corner below, so they
	// are normalised before use.
	const glm::vec3 position = glm::vec3(cameralookmatrix[3]);
	const glm::vec3 right = glm::normalize(glm::vec3(cameralookmatrix[0]));
	const glm::vec3 up = glm::normalize(glm::vec3(cameralookmatrix[1]));
	// A right handed camera looks down its own negative Z, which is why the third column
	// has to be negated to get the direction the player is actually facing.
	const glm::vec3 front = -glm::normalize(glm::vec3(cameralookmatrix[2]));

	// gCamera stores a vertical field of view in degrees and hands it to glm::perspective
	// as fovY, so the half height of a slice is tan(fov / 2) * distance and the half width
	// follows from the viewport's aspect ratio.
	const float halftan = tanf(glm::radians(fovdegrees) * 0.5f);
	const float nearhalfheight = halftan * neardistance;
	const float nearhalfwidth = nearhalfheight * aspect;
	const float farhalfheight = halftan * fardistance;
	const float farhalfwidth = farhalfheight * aspect;

	const glm::vec3 nearcenter = position + front * neardistance;
	const glm::vec3 farcenter = position + front * fardistance;

	outcorners[0] = nearcenter - right * nearhalfwidth - up * nearhalfheight;
	outcorners[1] = nearcenter + right * nearhalfwidth - up * nearhalfheight;
	outcorners[2] = nearcenter + right * nearhalfwidth + up * nearhalfheight;
	outcorners[3] = nearcenter - right * nearhalfwidth + up * nearhalfheight;
	outcorners[4] = farcenter - right * farhalfwidth - up * farhalfheight;
	outcorners[5] = farcenter + right * farhalfwidth - up * farhalfheight;
	outcorners[6] = farcenter + right * farhalfwidth + up * farhalfheight;
	outcorners[7] = farcenter - right * farhalfwidth + up * farhalfheight;
}

gShadowMap::~gShadowMap() {}

void gShadowMap::allocate(gLight* light, gCamera* camera, int width, int height) {
	// The Vulkan backend has no GL context and no shadowmap shader, so instead of
	// the depthfbo below it builds its own depth-only render target and pipeline.
	// Everything after that - the light matrices, the two render passes, the
	// enable/disable pairing - is shared with the OpenGL path.
	//
	// If the backend cannot provide one, isallocated stays false and every entry
	// point of this class turns into a no-op, so a game that asks for shadows
	// renders without them instead of crashing.
	if(renderer->isVulkan()) {
		if(!renderer->allocateShadowMap(width, height)) {
			gLogi("gShadowMap") << "The Vulkan backend could not allocate a shadow map; "
					<< "the scene will be drawn unshadowed.";
			return;
		}
		this->width = width;
		this->height = height;
		isallocated = true;
		this->camera = camera;
		this->light = light;
		lightposition = light->getPosition();
		setLightView(glm::lookAt(lightposition, glm::vec3(0.0f), glm::vec3(0.0, 1.0, 0.0)));
		setLightProjection(glm::ortho(-40.0f, 40.0f, -40.0f, 40.0f, 2.0f, 114.0f));
		return;
	}

	this->width = width;
	this->height = height;
	depthfbo.allocate(width, height, true);
	isallocated = true;
	this->camera = camera;
	this->light = light;
	lightposition = light->getPosition();
	setLightView(glm::lookAt(lightposition, glm::vec3(0.0f), glm::vec3(0.0, 1.0, 0.0)));
	setLightProjection(glm::ortho(-40.0f, 40.0f, -40.0f, 40.0f, 2.0f, 114.0f));
}

bool gShadowMap::isAllocated() const {
	return isallocated;
}

int gShadowMap::getWidth() const {
	return width;
}

int gShadowMap::getHeight() const {
	return height;
}

void gShadowMap::update() {
	lightposition = light->getPosition();
	// With camera fitting on, fitToCamera() decides both the view and the projection, so
	// the fixed look-at below would only be overwritten a line later. Without it, this is
	// the historical behaviour: aim the light at the world origin and keep whatever ortho
	// box the application asked for.
	if (camerafitenabled) {
		fitToCamera();
	} else {
		setLightView(glm::lookAt(lightposition, glm::vec3(0.0f), glm::vec3(0.0, 1.0, 0.0)));
	}
	renderpassno = 2;
	updateshadows = true;
}

// Fit one orthographic volume to one slice of the camera frustum, in the space of a light
// whose orientation is already fixed. This is the whole of the algorithm; everything else
// in this file is bookkeeping around how many times it is called and with what.
//
// The problem it solves is that a shadow map is a fixed number of texels spread over
// whatever slab of world the light's orthographic box covers. A box big enough to contain
// a whole level spends nearly all of those texels on geometry the player cannot see, and
// the handful that do land in view are enormous - which is why a static box produces both
// blocky shadows and a hard edge where the box runs out and shadows simply stop existing.
// Fitting the box to the camera's own frustum inverts that trade: every texel lands
// somewhere the player is looking, and the box can never be walked out of because it walks
// with them.
//
// tilewidth and tileheight are the texels this volume actually gets, which is the whole
// shadow map for a single fitted volume and one tile of the atlas for a cascade.
void gShadowMap::fitVolume(const glm::mat4& lightrotation, float neardistance, float fardistance,
		int tilewidth, int tileheight, glm::mat4& outprojection, float& outtexelworldsize) {
	const float aspect = (float)renderer->getWidth() / (float)renderer->getHeight();

	glm::vec3 corners[8];
	gShadowMapComputeFrustumCorners(camera->getLookMatrix(), camera->getFov(), aspect,
			neardistance, fardistance, corners);

	// A bounding sphere, not a bounding box. A box fitted tightly to the corners changes
	// size as the camera turns - the diagonal of a frustum is longer than its side - and a
	// box whose size changes has a texel size that changes with it, which reintroduces
	// exactly the crawling edges the snapping below exists to remove. The radius of the
	// enclosing sphere, by contrast, depends only on the shape of the slice and not on its
	// orientation, so it is constant for a given field of view, aspect and split distance,
	// and the volume merely slides around instead of breathing.
	//
	// Where the sphere is centred is worth getting right rather than taking the average of
	// the corners, which is what most implementations do. The frustum of a slice is a
	// truncated pyramid and its centroid sits well in front of the point that minimises the
	// enclosing radius; solving for the centre that puts the near and far corner rings on
	// the same sphere instead gives a radius six to thirteen per cent smaller for the
	// aspect ratios a phone and a desktop actually use, and every per cent of radius is a
	// per cent of texel size. That centre lies on the view axis at
	//
	//     c = (far + near) * (1 + a2) / 2,   a2 = tan(fov/2)^2 * (aspect^2 + 1)
	//
	// which for a long slice runs past the far plane, and is clamped there: beyond that
	// point the far ring alone determines the sphere and moving the centre further only
	// grows it. The radius is then measured against the actual corners rather than trusted
	// from the formula, so the clamp cannot produce a sphere that fails to contain them.
	const float halftangent = tanf(glm::radians(camera->getFov()) * 0.5f);
	const float a2 = halftangent * halftangent * (aspect * aspect + 1.0f);
	float centerdistance = (fardistance + neardistance) * (1.0f + a2) * 0.5f;
	if (centerdistance > fardistance) centerdistance = fardistance;
	if (centerdistance < neardistance) centerdistance = neardistance;
	const glm::vec3 cameraposition = glm::vec3(camera->getLookMatrix()[3]);
	const glm::vec3 camerafront = -glm::normalize(glm::vec3(camera->getLookMatrix()[2]));
	glm::vec3 center = glm::vec3(lightrotation
			* glm::vec4(cameraposition + camerafront * centerdistance, 1.0f));

	glm::vec3 lightspacecorners[8];
	float radius = 0.0f;
	for (int i = 0; i < 8; i++) {
		lightspacecorners[i] = glm::vec3(lightrotation * glm::vec4(corners[i], 1.0f));
		const float distance = glm::length(lightspacecorners[i] - center);
		if (distance > radius) radius = distance;
	}
	// Rounding the radius up to a whole hundredth stops the last bits of floating point
	// noise in the corner arithmetic from jittering the texel size from frame to frame.
	// The texel size has to be identical between frames or the snapping below snaps to a
	// slightly different grid each time, which is the very thing it exists to prevent.
	radius = ceilf(radius * 100.0f) / 100.0f;
	if (radius < 0.01f) radius = 0.01f;

	// The border is why the divisors are not simply the tile's size. A ring of texels on
	// each side is deliberately left outside the fitted region and never written to, so
	// that the map carries an unwritten frame at the clear depth. What that frame is for is
	// explained at the scissor call in enable(); here it only means the fitted region has
	// to be mapped onto the inner rectangle, so the texel size is measured against that
	// inner size and the box is then grown by the border again.
	const float texelsizex = (2.0f * radius) / (float)(tilewidth - 2 * fitborder);
	const float texelsizey = (2.0f * radius) / (float)(tileheight - 2 * fitborder);

	// Texel snapping, which is not optional. A shadow map quantises the world into texels,
	// and if the grid those texels sit on drifts as the camera moves then every shadow edge
	// in the scene re-quantises a little differently each frame and boils. Snapping the
	// centre of the volume onto whole multiples of the texel size nails the grid to fixed
	// world positions: the volume then only ever moves in whole-texel steps, each texel
	// keeps covering the same patch of world from one frame to the next, and the edges hold
	// still. This is the single difference between a fitted shadow map that looks better
	// than a static one and a fitted shadow map that looks considerably worse.
	//
	// Note that this works only because the light view is a pure rotation shared by every
	// volume: the light-space coordinate a world point maps to is then independent of where
	// the camera is, so "a whole number of texels" means the same thing every frame.
	center.x = floorf(center.x / texelsizex) * texelsizex;
	center.y = floorf(center.y / texelsizey) * texelsizey;

	const float halfextentx = radius + (float)fitborder * texelsizex;
	const float halfextenty = radius + (float)fitborder * texelsizey;

	// The depth range. glm::lookAt points the view down its own negative Z, so a point
	// further along the light's direction has a more negative Z, and glm::ortho wants
	// positive distances - hence the negations. The near plane is pushed back towards the
	// light by the extrusion distance so that casters standing between the light and the
	// visible slab are still rasterised into the map; without that, a wall just off screen
	// would be clipped away and its shadow would vanish from the middle of the view. The
	// far plane gets a token unit of slack purely so a receiver lying exactly on the
	// boundary is not clipped by a rounding error.
	//
	// Fitting the depth range this tightly is worth as much as fitting the box. The
	// comparison in the shader happens in normalised depth, so what a given bias forgives
	// in world units is proportional to the depth of this frustum: a range of thirty units
	// instead of a hundred and twenty makes the same bias four times finer, and that is
	// what stops small shadows being swallowed by it.
	float minz = lightspacecorners[0].z;
	float maxz = lightspacecorners[0].z;
	for (int i = 1; i < 8; i++) {
		if (lightspacecorners[i].z < minz) minz = lightspacecorners[i].z;
		if (lightspacecorners[i].z > maxz) maxz = lightspacecorners[i].z;
	}
	//
	// Both ends are rounded outwards to whole world units. The depth range derived from the
	// corners is tighter than one derived from the bounding sphere - by roughly half, which
	// is worth having - but unlike the sphere it does change as the camera turns, and a
	// depth range that changes continuously means a normalised bias whose world meaning
	// changes continuously with it, so acne can fade in and out as the player looks around.
	// Quantising the range to whole units leaves it tight but makes it change in visible
	// steps only, which in practice is not visible at all.
	const float nearz = floorf(-maxz - shadowcasterextrusion);
	const float farz = ceilf(-minz + 1.0f);

	outprojection = glm::ortho(center.x - halfextentx, center.x + halfextentx,
			center.y - halfextenty, center.y + halfextenty, nearz, farz);
	// Reported in world units per texel so that a shader can scale its bias and its normal
	// offset by it. Every cascade has a different one, and a bias tuned for the sharpest
	// cascade will acne badly in the coarsest one if it is not scaled.
	outtexelworldsize = texelsizex > texelsizey ? texelsizex : texelsizey;
}

// Refit everything to what the camera can currently see: the legacy single volume that
// gets published as lightmatrix, and - when more than one cascade has been asked for - the
// per-cascade volumes alongside it.
//
// The two are computed independently on purpose. lightmatrix is what both backends consume
// today and what the OpenGL shaders will always consume, so it must go on being a single
// volume covering the whole shadow distance no matter what the cascade settings say.
// Turning cascades on therefore cannot disturb the path that already works; it only adds
// data next to it. One extra fit per frame is a few dozen floating point operations, which
// is not a price worth optimising away for that guarantee.
void gShadowMap::fitToCamera() {
	if (!camerafitenabled || camera == nullptr || light == nullptr) return;
	if (width <= 2 * fitborder || height <= 2 * fitborder) return;

	// The direction the light travels. It is derived from the light's position rather than
	// from gLight::getDirection() on purpose: the pre-existing behaviour of this class was
	// lookAt(lightposition, origin), so the sun's angle in every game that already uses
	// shadows is a consequence of where that game put the light, and reading the light's
	// orientation instead would silently swing every existing shadow to a new angle. This
	// keeps the angle identical to what it was and changes only the coverage, which is what
	// makes the fitted mode a strict improvement rather than a different look. A light
	// sitting exactly on the origin has no such direction to recover, and only then is its
	// orientation consulted.
	glm::vec3 lightdirection = -lightposition;
	if (glm::dot(lightdirection, lightdirection) < 0.000001f) lightdirection = light->getDirection();
	if (glm::dot(lightdirection, lightdirection) < 0.000001f) lightdirection = glm::vec3(0.0f, -1.0f, 0.0f);
	lightdirection = glm::normalize(lightdirection);

	// glm::lookAt needs an up vector that is not parallel to the view direction, and a sun
	// hanging straight overhead is exactly the degenerate case, so the world up is swapped
	// for the world forward when the light is close to vertical.
	glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
	if (fabsf(lightdirection.y) > 0.99f) up = glm::vec3(0.0f, 0.0f, 1.0f);

	// A rotation-only light view: the eye stays at the world origin, because an
	// orthographic projection has no eye point in any meaningful sense - only an
	// orientation and a box. Keeping the translation out of the view and inside the box is
	// what lets every cascade share one view matrix, and, more importantly, it is what
	// makes the texel snapping in fitVolume() well defined.
	const glm::mat4 lightrotation = glm::lookAt(glm::vec3(0.0f), lightdirection, up);

	const float nearplane = camera->getNearClip();
	// Zero means "cover everything the camera can see"; anything the caller does ask for is
	// still capped at the far clip, because a shadow volume reaching past the point where
	// geometry is culled would be pure waste.
	float fardistance = shadowdistance > 0.0f ? shadowdistance : camera->getFarClip();
	if (fardistance > camera->getFarClip()) fardistance = camera->getFarClip();
	if (fardistance <= nearplane) fardistance = nearplane + 1.0f;

	glm::mat4 projection;
	float texelworldsize;
	fitVolume(lightrotation, nearplane, fardistance, width, height, projection, texelworldsize);
	setLightView(lightrotation);
	setLightProjection(projection);
	legacytexelworldsize = texelworldsize;

	// The cascades. A single fitted volume is already a large improvement over a static
	// one, but it still spends its texels uniformly over the whole shadow distance while
	// the eye's demand for detail is anything but uniform: a shadow ten metres away covers
	// a hundred times more screen area than the same shadow a hundred metres away, and
	// wants a hundred times the texel density. Splitting the range and giving each slice
	// its own map is the only way to serve both from a fixed budget.
	const int tilewidth = width / getCascadeAtlasColumns();
	const int tileheight = height / getCascadeAtlasRows();
	if (tilewidth <= 2 * fitborder || tileheight <= 2 * fitborder) return;

	// Where to cut. A uniform split wastes the sharp cascades on distance the eye does not
	// care about; a purely logarithmic split - which is what perspective projection would
	// actually ask for - crams the first cascade into the few centimetres in front of the
	// near clip plane and is useless in practice, because the near clip plane is a
	// hundredth of a unit here and the logarithm is dominated by it. The practical split
	// scheme interpolates between the two and lets the application pick where on that line
	// it wants to sit.
	//
	// The logarithmic term is additionally computed from a floor on the near distance
	// rather than the camera's own near clip, for exactly that reason: with a near clip of
	// 0.01 and a shadow distance of 60 the first split would otherwise land at nine
	// centimetres, and a whole quarter of the shadow map would be spent on the inside of
	// the player's own model.
	float lognear = fardistance * 0.005f;
	if (lognear < nearplane) lognear = nearplane;
	const float range = fardistance - nearplane;
	const float ratio = fardistance / lognear;

	float slicenear = nearplane;
	for (int i = 0; i < cascadenum; i++) {
		const float fraction = (float)(i + 1) / (float)cascadenum;
		const float logsplit = lognear * powf(ratio, fraction);
		const float uniformsplit = nearplane + range * fraction;
		float slicefar = cascadesplitlambda * logsplit + (1.0f - cascadesplitlambda) * uniformsplit;
		if (i == cascadenum - 1) slicefar = fardistance;
		if (slicefar <= slicenear) slicefar = slicenear + 0.01f;

		// Each slice is fitted with its own overlap back towards the camera. The overlap is
		// what the fragment shader cross-fades over: without it the switch from one cascade
		// to the next is a hard line across the ground where the texel size, and with it
		// the exact shape of every filtered shadow edge, changes in one pixel. The band has
		// to be inside both cascades for the blend to have two values to mix, so the slice
		// starts a little before the previous one ended.
		const float blendback = (slicefar - slicenear) * cascadeblendratio;
		float fitnear = slicenear - blendback;
		if (fitnear < nearplane) fitnear = nearplane;

		glm::mat4 cascadeprojection;
		fitVolume(lightrotation, fitnear, slicefar, tilewidth, tileheight, cascadeprojection,
				cascadetexelworldsize[i]);
		cascadematrix[i] = cascadeprojection * lightrotation;
		cascadesplitdistance[i] = slicefar;
		slicenear = slicefar;
	}
	for (int i = cascadenum; i < MAXCASCADENUM; i++) {
		cascadematrix[i] = cascadematrix[cascadenum - 1];
		cascadesplitdistance[i] = fardistance;
		cascadetexelworldsize[i] = cascadetexelworldsize[cascadenum - 1];
	}
}

void gShadowMap::setLight(gLight* light) {
	this->light = light;
	lightposition = light->getPosition();
	// A new light means a new direction and therefore a new light-space basis, so the fit
	// is redone on the spot instead of being left a frame out of date.
	if (camerafitenabled) fitToCamera();
	else setLightView(glm::lookAt(lightposition, glm::vec3(0.0f), glm::vec3(0.0, 1.0, 0.0)));
}

void gShadowMap::setCamera(gCamera* camera) {
	this->camera = camera;
	if (camerafitenabled) fitToCamera();
}

gLight* gShadowMap::getLight() const {
	return light;
}

gCamera* gShadowMap::getCamera() const {
	return camera;
}

void gShadowMap::activate() {
	isactivated = true;
	renderpassnum = 2;
	updateshadows = true;
}

void gShadowMap::deactivate() {
	renderpassnum = 1;
	updateshadows = false;
	disable();
	isactivated = false;
}

bool gShadowMap::isActivated() const {
	return isactivated;
}

void gShadowMap::enable() {
	G_PROFILE_ZONE_SCOPED_N("gShadowMap::enable()");
	if (!isallocated || !isactivated) return;

	isenabled = true;
	isshadowmappingenabled = true;

	// Refit here rather than in update(). update() is a method the application has to
	// remember to call every frame and most do not - the shadow map is set up once and then
	// left alone, which is exactly why a static light volume survived this long. enable()
	// is called by every frame that draws shadows at all, by definition, so it is the only
	// hook that makes a per-frame fit reliable.
	//
	// Only on pass 0. enable() runs once per render pass, and the depth pass and the
	// shading pass have to agree on where the light was to the last bit: refitting between
	// them would fill the map from one volume and then look fragments up in another, which
	// puts every shadow in the wrong place.
	if (camerafitenabled && renderpassno == 0) fitToCamera();

	// The Vulkan path does not switch shaders or bind an FBO here: which pass is
	// being recorded is decided by the frame loop, which opens the shadow render
	// pass for pass 0 and the screen one for pass 1. All this has to do is keep the
	// backend's copy of the light transform current, so the depth pass and the
	// shading pass agree on where the light is.
	if (renderer->isVulkan()) {
		renderer->setShadowMapState(true, lightmatrix, lightposition,
				renderer->isSoftShadowsEnabled());
		return;
	}

	if (updateshadows && renderpassno == 0) {
		glViewport(0, 0, depthfbo.getWidth(), depthfbo.getHeight());
		renderer->getShadowmapShader()->use();
		renderer->getShadowmapShader()->setMat4("lightMatrix", lightmatrix);
		depthfbo.bind();
	//	glViewport(0, 0, width, height);
		renderer->clearScreen(false, true);
		// Keep a ring of texels around the edge of the map unwritten, and this is the only
		// reason the fitted projection reserves one.
		//
		// The depth attachment of this FBO is created with clamp-to-edge wrapping (see
		// gTexture's GL_DEPTH_COMPONENT branch), and color_frag.glsl's calculateShadow()
		// guards against a fragment falling outside the light's frustum in depth but not in
		// x or y - it checks projCoords.z and then samples projCoords.xy unconditionally.
		// With the old, enormous static box that was survivable, because the outermost
		// texels of a box that covered a whole level were almost always empty sky and
		// compared as lit. A fitted box is small by design, its edge texels hold real
		// geometry, and clamp-to-edge would then stretch those few depths across the entire
		// world outside the fit - painting phantom shadows over everything past the shadow
		// distance.
		//
		// The fix has to live on this side, because the OpenGL shaders are not ours to
		// change. A scissor rectangle two texels inside the map means nothing is ever
		// rasterised into that ring, the clear leaves it at depth 1.0 - glClear is bounded
		// by the scissor box, so it is enabled after the clear, not before - and every
		// lookup that lands outside the fitted region now reads the far plane and comes
		// back lit. That is precisely the old behaviour outside the volume, restored, for
		// the cost of four texels out of four thousand.
		if (camerafitenabled && depthfbo.getWidth() > 2 * fitborder && depthfbo.getHeight() > 2 * fitborder) {
			glEnable(GL_SCISSOR_TEST);
			glScissor(fitborder, fitborder, depthfbo.getWidth() - 2 * fitborder,
					depthfbo.getHeight() - 2 * fitborder);
		}
	} else {
		// Unconditionally, not only when fitting is on: this is the one place guaranteed to
		// run every frame that the shadow pass also ran, so it is where the scissor test has
		// to be given back in the state the rest of the engine expects to find it.
		glDisable(GL_SCISSOR_TEST);
		glViewport(0, 0, renderer->getScreenWidth(), renderer->getScreenHeight());
		renderer->getColorShader()->use();
		renderer->getColorShader()->setInt("aUseShadowMap", 1);
		renderer->getColorShader()->setVec3("lightPos", lightposition);
		renderer->getColorShader()->setInt("shadowMap", shadowmaptextureslot);

		renderer->bindTexture(depthfbo.getTextureId(), shadowmaptextureslot);
		renderpassno = 1;
	}
}

void gShadowMap::disable() {
	G_PROFILE_ZONE_SCOPED_N("gShadowMap::disable()");
	if (!isallocated || !isactivated || renderpassno > 0) return;

	isenabled = false;
	isshadowmappingenabled = false;

	// Nothing to unbind on Vulkan: there is no bound FBO, and the render pass is
	// closed by the frame loop that opened it.
	if (renderer->isVulkan()) return;

	// Paired with the scissor enable in enable(). It is dropped here as well as at the top
	// of the shading pass because this is the earlier of the two, and anything the
	// application draws between the passes would otherwise be clipped to the shadow map's
	// inner rectangle.
	glDisable(GL_SCISSOR_TEST);
	depthfbo.unbind();
}

bool gShadowMap::isEnabled() const {
	return isenabled;
}

void gShadowMap::setLightProjection(glm::mat4 lightProjection) {
	lightprojection = lightProjection;
	lightmatrix = lightprojection * lightview;
}

void gShadowMap::setLightProjection(float leftx, float rightx, float fronty, float backy, float nearz, float farz) {
	lightprojection = glm::ortho(leftx, rightx, fronty, backy, nearz, farz);
	lightmatrix = lightprojection * lightview;
}

glm::mat4 gShadowMap::getLightProjection() const {
	return lightprojection;
}

void gShadowMap::setLightView(glm::mat4 lightView) {
	lightview = lightView;
	lightmatrix = lightprojection * lightview;
}

glm::mat4 gShadowMap::getLightView() const {
	return lightview;
}

glm::mat4 gShadowMap::getLightMatrix() const {
	return lightmatrix;
}

void gShadowMap::enableCameraFit(float shadowDistance) {
	camerafitenabled = true;
	shadowdistance = shadowDistance;
	// Fit immediately rather than waiting for the first shadow pass, so that a caller that
	// reads getLightMatrix() straight after switching the mode on gets the fitted matrix
	// and not the box that happened to be there beforehand.
	fitToCamera();
}

void gShadowMap::disableCameraFit() {
	// Whatever matrices the fit last produced are left in place on purpose. They are a
	// valid volume, they are the one the last frame was drawn with, and clearing them to
	// some default would be a visible jump for no reason; an application turning the mode
	// off almost certainly means to set its own box on the next line anyway.
	camerafitenabled = false;
}

bool gShadowMap::isCameraFitEnabled() const {
	return camerafitenabled;
}

void gShadowMap::setShadowDistance(float distance) {
	shadowdistance = distance;
}

float gShadowMap::getShadowDistance() const {
	return shadowdistance;
}

void gShadowMap::setShadowCasterExtrusion(float distance) {
	shadowcasterextrusion = distance;
}

float gShadowMap::getShadowCasterExtrusion() const {
	return shadowcasterextrusion;
}

float gShadowMap::getTexelWorldSize() const {
	return legacytexelworldsize;
}

void gShadowMap::setCascadeNum(int num) {
	if (num < 1) num = 1;
	if (num > MAXCASCADENUM) num = MAXCASCADENUM;
	cascadenum = num;
}

int gShadowMap::getCascadeNum() const {
	return cascadenum;
}

void gShadowMap::setCascadeSplitLambda(float lambda) {
	cascadesplitlambda = glm::clamp(lambda, 0.0f, 1.0f);
}

float gShadowMap::getCascadeSplitLambda() const {
	return cascadesplitlambda;
}

void gShadowMap::setCascadeBlendRatio(float ratio) {
	// Capped well below a half: an overlap approaching the whole of a cascade's range would
	// mean the sharp cascade is being cross-faded away over most of the ground it was
	// created to cover, which throws away the resolution the split bought in the first
	// place.
	cascadeblendratio = glm::clamp(ratio, 0.0f, 0.4f);
}

float gShadowMap::getCascadeBlendRatio() const {
	return cascadeblendratio;
}

const glm::mat4& gShadowMap::getCascadeMatrix(int index) const {
	if (index < 0) index = 0;
	if (index >= MAXCASCADENUM) index = MAXCASCADENUM - 1;
	return cascadematrix[index];
}

float gShadowMap::getCascadeSplitDistance(int index) const {
	if (index < 0) index = 0;
	if (index >= MAXCASCADENUM) index = MAXCASCADENUM - 1;
	return cascadesplitdistance[index];
}

float gShadowMap::getCascadeTexelWorldSize(int index) const {
	if (index < 0) index = 0;
	if (index >= MAXCASCADENUM) index = MAXCASCADENUM - 1;
	return cascadetexelworldsize[index];
}

int gShadowMap::getCascadeAtlasColumns() const {
	return cascadenum > 1 ? 2 : 1;
}

int gShadowMap::getCascadeAtlasRows() const {
	// Two rows only once there are more than two cascades, so that two cascades take the
	// left and right halves of the image rather than a quarter each and leave half of it
	// unused. Three cascades leave one quarter idle, which is the price of not resizing an
	// image that is already allocated by the time any of this is decided.
	return cascadenum > 2 ? 2 : 1;
}

int gShadowMap::getCascadeTileWidth() const {
	return width / getCascadeAtlasColumns();
}

int gShadowMap::getCascadeTileHeight() const {
	return height / getCascadeAtlasRows();
}

glm::vec4 gShadowMap::getCascadeAtlasRect(int index) const {
	if (index < 0) index = 0;
	if (index >= MAXCASCADENUM) index = MAXCASCADENUM - 1;
	const int columns = getCascadeAtlasColumns();
	const int rows = getCascadeAtlasRows();
	const float scalex = 1.0f / (float)columns;
	const float scaley = 1.0f / (float)rows;
	const int column = index % columns;
	const int row = (index / columns) % rows;
	return glm::vec4((float)column * scalex, (float)row * scaley, scalex, scaley);
}

gFbo& gShadowMap::getDepthFbo() {
	return depthfbo;
}


