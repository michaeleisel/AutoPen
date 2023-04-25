# AutoPen
## A faster version of codesign

### Introduction

For large projects, the codesigning phase can add a significant amount of time to incremental builds. This project provides an alternate version of `codesign` that is significantly faster. Under the hood, it essentially invokes `codesign` itself but with special runtime modifications made to `codesign` that cause it to run much faster.


***Note***: it is only intended for debug builds, to make debugging faster.

### Usage

To integrate AutoPen, use the following steps, applied specifically to the debug builds you want to use AutoPen for:

- Get the pre-built binary for the latest release from [here](https://github.com/michaeleisel/zld/releases/latest)
- In the Xcode build settings, add `--digest-algorithm=sha1` to "Other Code Signing Flags" . This is required for AutoPen to work, but also substantially reduces codesigning time in its own right. Also consider adding `--resource-rules /path/to/rules.xml` for even more speed. More info on these flags is [here](https://eisel.me/signing).
- In Xcode build settings, add a user-defined setting of `CODE_SIGNING_ALLOWED` set to `NO`. This will disable Xcode's default codesigning in order to use AutoPen instead.
- Add a "run script" build phase after all other build phases that runs AutoPen with the same exact flags that the default codesigning step previously did. E.g., `/path/to/AutoPen -s ${EXPANDED_CODE_SIGN_IDENTITY} --timestamp\=none --generate-entitlement-der --entitlements /path/to/entitlements.xcent ${OTHER_CODE_SIGN_FLAGS} ${CODESIGNING_FOLDER_PATH}`. Note that the `entitlements.xcent` file will change as the app's entitlements are changed, and the full set of flags that Xcode is adding for code signing may change across Xcode versions.

### Implementation Details

This tool primarily addresses the bottleneck of signing the main binary, which involves the expensive operations of both reading the file from disk and hashing each 4kb chunk of it. In `codesign` this is done by reading each chunk and then signing it on a single thread. With AutoPen, it instead parallelizes the hash computation and does it while also reading further 4kb chunks from disk.

In order to do this, the tool doesn't actually do any of the codesigning on its own, but instead it does these steps:
1. Swap out some key library functions to alter `codesign`'s behavior
2. Load `/usr/bin/codesign` as a dynamic library
3. Invoke its main function manually

This method is necessary necessary in order to circumvent `codesign`'s hardening against `DYLD_INSERT_LIBRARIES` and to avoid invokin g a self-signed version of `codesign`, which won't work because arm64e executables like `codesign` can only be run if they're Apple-signed. Credit to @Siguza for suggesting to do it this way. Why not simply modify an existing codesigning tool like `rcodesign`? Because the tools that I've seen aren't drop-in replacements (they don't support the exact same flags in the exact same way) and because they don't, in my experience, produce byte-for-byte identical signed binaries.

In terms of the runtime hooking, AutoPen hooks a number of libc and Security.framework functions such that they use AutoPen's precomputed parallel hashes as necessary rather than doing any actual work.