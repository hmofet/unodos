#!/bin/bash
# Generate build number from git commit hash
# Use HEAD (current commit) for the build number
# This represents the last code change, not the binary update
BUILD=$(git rev-parse --short HEAD 2>/dev/null || echo "dev")
echo "Build: $BUILD"
echo -n "$BUILD" > kernel/BUILD
