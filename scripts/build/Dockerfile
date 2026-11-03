# The image that builds the application (see ../../docker-compose.yml). It only has what the build needs to get going: Node (the
# build script, the TypeScript compiler, the bundler), Python and git (to fetch Emscripten and Box3D), rsync (to copy the sources).
# Emscripten, Box3D, the Node packages and the compiled objects are not in the image: build-in-container.sh puts them in the named
# volumes of the compose file the first time and finds them there afterwards.
FROM node:24-bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends python3 git rsync xz-utils bzip2 curl ca-certificates make \
 && rm -rf /var/lib/apt/lists/*

COPY build-in-container.sh /usr/local/bin/build-in-container.sh
RUN chmod +x /usr/local/bin/build-in-container.sh

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/build-in-container.sh"]
