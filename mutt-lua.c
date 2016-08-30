/* Copyright (C) 2016 Richard Russon <rich@flatcap.org>
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "mutt.h"
#include "globals.h"

int
lua_test (void)
{
  lua_State *l;

  if (!LuaScript)
    return 0;

  /* initialise Lua */
  l = luaL_newstate();
  if (!l)
    return 0;

  /* load Lua base libraries */
  luaL_openlibs (l);

  if (luaL_dofile (l, LuaScript) == 0)
  {
    /* one value on the stack */
    if (lua_gettop (l) == 1)
    {
      mutt_message ("lua returned: %lld", lua_tointeger (l, 1));
    }
  }
  else
  {
    mutt_error ("error running lua script");
  }

  /* cleanup Lua */
  lua_close (l);

  return 1;
}

