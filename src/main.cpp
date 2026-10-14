// fm3 - Forza Motorsport 3 recompiled for PC.

#include "generated/default/fm3_init.h"

#include "fm3_app.h"

std::unique_ptr<rex::ui::WindowedApp> Fm3App::Create(rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<Fm3App>(new Fm3App(ctx, "fm3", PPCImageConfig));
}

REX_DEFINE_APP(fm3, Fm3App::Create)
