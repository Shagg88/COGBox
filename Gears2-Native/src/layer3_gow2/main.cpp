// gow2 - ReXGlue Recompiled Project (rexglue init template). Layer 3.

#if defined(GOW2_TU6) && GOW2_TU6  // notes/63: the Title Update 6 build (tools\build.ps1 -Tu6)
#include "generated/tu6/gow2_init.h"
#else
#include "generated/default/gow2_init.h"
#endif

#include "gow2_app.h"

REX_DEFINE_APP(gow2, Gow2App::Create)
