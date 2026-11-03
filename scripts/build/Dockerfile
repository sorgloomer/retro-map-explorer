# syntax=docker/dockerfile:1
# Builds the application (see ../../docker-compose.yml). The sources are not mounted from the host but sent to the builder with the build
# context (the Docker client reads them, so they can be anywhere the client can: a network drive too), and the output is written back to
# the client's folder, so that nothing needs to be shared with the engine. The tools (Emscripten, Box3D, the compiled objects) and npm's
# download cache are BuildKit cache mounts: they stay in the builder between builds, which then compile only what changed.
FROM node:24-bookworm-slim AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends python3 git xz-utils bzip2 curl ca-certificates make \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work
COPY package.json package-lock.json tsconfig.json ./
COPY scripts/ scripts/
COPY src/ src/

ARG FLAGS=""
ENV RME_TOOLS_DIR=/opt/rme-tools RME_DIST_DIR=/out
RUN --mount=type=cache,target=/opt/rme-tools \
    --mount=type=cache,target=/root/.npm \
    node scripts/build.mjs $FLAGS

# only the output leaves the build
FROM scratch AS dist
COPY --from=build /out/ /
