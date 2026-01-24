#!/bin/bash
# Generate build number from git commit hash
BUILD=$(git rev-parse --short HEAD 2>/dev/null || echo "dev")
echo "Build: $BUILD"
echo -n "$BUILD" > kernel/BUILD
