// RGFW implementation lives in its own translation unit so its platform /
// objc-runtime code stays isolated from glad's GL symbols (which main.c pulls
// in). Compiled as C; RGFW auto-detects the platform backend from the OS macros.
#define RGFW_IMPLEMENTATION
#define RGFW_OPENGL
#define RGFWDEF // external linkage (match main.c); RGFW's default is inline
#include <RGFW.h>
