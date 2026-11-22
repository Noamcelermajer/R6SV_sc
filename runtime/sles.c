/* OpenSL ES is not used by this game: Vox takes its JNI AudioTrack path (g_androidApiLevel = 3, libOpenSLES is not
 * linked), see media.c. Kept as an empty registration so core.c's call still links; remove the call and this file together. */
#include "rt.h"

void rt_sles_register(void) {}
