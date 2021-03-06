//
//  scene.c
//  C-ray
//
//  Created by Valtteri Koskivuori on 28/02/2015.
//  Copyright © 2015-2020 Valtteri Koskivuori. All rights reserved.
//

#include "../includes.h"
#include "scene.h"

#include "../utils/timer.h"
#include "../utils/loaders/sceneloader.h"
#include "../utils/logging.h"
#include "image/imagefile.h"
#include "../renderer/renderer.h"
#include "image/texture.h"
#include "image/hdr.h"
#include "camera.h"
#include "vertexbuffer.h"
#include "../accelerators/bvh.h"
#include "tile.h"
#include "mesh.h"
#include "poly.h"
#include "../utils/platform/thread.h"
#include "../utils/args.h"
#include "../utils/ui.h"

static void transformMeshes(struct world *scene) {
	logr(info, "Running transforms: ");
	struct timeval timer = {0};
	startTimer(&timer);
	for (int i = 0; i < scene->meshCount; ++i) {
		transformMesh(&scene->meshes[i]);
	}
	printSmartTime(getMs(timer));
	printf("\n");
}

//TODO: Parallelize this task
static void computeAccels(struct mesh *meshes, int meshCount) {
	logr(info, "Computing BVHs: ");
	struct timeval timer = {0};
	startTimer(&timer);
	for (int i = 0; i < meshCount; ++i) {
		meshes[i].bvh = buildBottomLevelBvh(meshes[i].polygons, meshes[i].polyCount);
	}
	printSmartTime(getMs(timer));
	printf("\n");
}

static void computeTopLevelBvh(struct world *scene) {
	logr(info, "Computing top-level BVH: ");
	struct timeval timer = {0};
	startTimer(&timer);
	scene->topLevel = buildTopLevelBvh(scene->meshes, scene->meshCount);
	printSmartTime(getMs(timer));
	printf("\n");
}

static void printSceneStats(struct world *scene, unsigned long long ms) {
	logr(info, "Scene construction completed in ");
	printSmartTime(ms);
	unsigned polys = 0;
	for (int m = 0; m < scene->meshCount; ++m)
		polys += scene->meshes[m].polyCount;
	printf("\n");
	logr(info, "Totals: %iV, %iN, %iT, %iP, %iS, %iM\n",
		   vertexCount,
		   normalCount,
		   textureCount,
		   polys,
		   scene->sphereCount,
		   scene->meshCount);
}

static void checkAndSetCliOverrides(struct renderer *r) {
	//Update threadCount if it's overridden
	if (isSet("thread_override")) {
		int threads = intPref("thread_override");
		if (r->prefs.threadCount != threads) {
			logr(info, "Overriding thread count to %i\n", threads);
			r->prefs.threadCount = threads;
			r->prefs.fromSystem = false;
		}
	}
	
	if (isSet("samples_override")) {
		int samples = intPref("samples_override");
		logr(info, "Overriding sample count to %i\n", samples);
		r->prefs.sampleCount = samples;
	}
	
	//Update image dimensions if it's overridden
	if (isSet("dims_override")) {
		int width = intPref("dims_width");
		int height = intPref("dims_height");
		logr(info, "Overriding image dimensions to %ix%i\n", width, height);
		r->prefs.imageWidth = width;
		r->prefs.imageHeight = height;
	}
	
	if (isSet("tiledims_override")) {
		int width = intPref("tile_width");
		int height = intPref("tile_height");
		logr(info, "Overriding tile  dimensions to %ix%i\n", width, height);
		r->prefs.tileWidth = width;
		r->prefs.tileHeight = height;
	}
}

//Split scene loading and prefs?
//Load the scene, allocate buffers, etc
//FIXME: Rename this func and take parseJSON out to a separate call.
int loadScene(struct renderer *r, char *input) {
	
	struct timeval timer = {0};
	startTimer(&timer);
	
	//Build the scene
	switch (parseJSON(r, input)) {
		case -1:
			logr(warning, "Scene builder failed due to previous error.\n");
			return -1;
		case 4:
			logr(warning, "Scene debug mode enabled, won't render image.\n");
			return -1;
		case -2:
			//JSON parser failed
			return -1;
		default:
			break;
	}
	
	checkAndSetCliOverrides(r);
	transformCameraIntoView(r->scene->camera);
	transformMeshes(r->scene);
	computeAccels(r->scene->meshes, r->scene->meshCount);
	computeTopLevelBvh(r->scene);
	printSceneStats(r->scene, getMs(timer));
	
	//Quantize image into renderTiles
	r->state.tileCount = quantizeImage(&r->state.renderTiles,
									   r->prefs.imageWidth,
									   r->prefs.imageHeight,
									   r->prefs.tileWidth,
									   r->prefs.tileHeight,
									   r->prefs.tileOrder);
	
	// Some of this stuff seems like it should be in newRenderer(), but notice
	// how they depend on r->prefs, which is populated by parseJSON
	
	//Compute the focal length for the camera
	computeFocalLength(r->scene->camera, r->prefs.imageWidth);
	
	//Allocate memory for render buffer
	//Render buffer is used to store accurate color values for the renderers' internal use
	r->state.renderBuffer = newTexture(float_p, r->prefs.imageWidth, r->prefs.imageHeight, 3);
	
	//Allocate memory for render UI buffer
	//This buffer is used for storing UI stuff like currently rendering tile highlights
	r->state.uiBuffer = newTexture(char_p, r->prefs.imageWidth, r->prefs.imageHeight, 4);
	
	//Print a useful warning to user if the defined tile size results in less renderThreads
	if (r->state.tileCount < r->prefs.threadCount) {
		logr(warning, "WARNING: Rendering with a less than optimal thread count due to large tile size!\n");
		logr(warning, "Reducing thread count from %i to %i\n", r->prefs.threadCount, r->state.tileCount);
		r->prefs.threadCount = r->state.tileCount;
	}
	return 0;
}

//Free scene data
void destroyScene(struct world *scene) {
	if (scene) {
		destroyHDRI(scene->hdr);
		destroyCamera(scene->camera);
		for (int i = 0; i < scene->meshCount; ++i) {
			destroyMesh(&scene->meshes[i]);
		}
		destroyBvh(scene->topLevel);
		free(scene->meshes);
		free(scene->spheres);
		free(scene);
	}
}
