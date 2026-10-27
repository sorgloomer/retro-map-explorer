// The options as the page's controls set them, before they are mapped to the renderer's units. The page writes this struct;
// toRender() turns it into the RenderOptions the rendering pipeline reads.
#pragma once
#include <cmath>
#include "scene/renderer.h"

struct UiOptions {
    float exposure = 1.35f;
    float haloExposure = 1.35f;
    bool textured = true;
    float powerupScale = 0.0f;  // slider 0..1: log10 of the powerup size factor
    float lineWidth = 3.0f;
    float xrayStrength = 1.0f;
    bool showGeometry = true;
    bool wireframe = false;
    bool backFaces = false;
    bool beams = false;
    bool powerupMeshes = true;

    RenderOptions toRender() const {
        RenderOptions r;
        r.exposure = exposure;
        r.haloExposure = haloExposure;
        r.textured = textured;
        r.powerupScale = std::pow(11.0f, (float)powerupScale) - 1.0f;
        r.lineWidth = lineWidth;
        r.xrayStrength = xrayStrength;
        r.showGeometry = showGeometry;
        r.wireframe = wireframe;
        r.backFaces = backFaces;
        r.beams = beams;
        r.powerupMeshes = powerupMeshes;
        return r;
    }
};
