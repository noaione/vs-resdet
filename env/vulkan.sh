#!/usr/bin/env bash

# The machine's Vulkan SDK/loader installation is in /usr/local. Keeping this
# in a project script avoids silently changing the user's shell startup files.
export VULKAN_SDK="${VULKAN_SDK:-/usr/local}"
export PATH="$VULKAN_SDK/bin:$PATH"
export DYLD_LIBRARY_PATH="$VULKAN_SDK/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
export VK_ICD_FILENAMES="$VULKAN_SDK/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_LAYER_PATH="$VULKAN_SDK/share/vulkan/explicit_layer.d"
