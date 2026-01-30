/*
** Object JSON de/serialization.
** Copyright (C) 2005-2025 BeamNG GmbH. See Copyright Notice in luajit.h
*/

#ifndef _LJ_SERIALIZE_JSON_H
#define _LJ_SERIALIZE_JSON_H

#include "lj_obj.h"
#include "lj_buf.h"

#if LJ_HASBUFFER

#define LJ_SERIALIZE_DEPTH    100    /* Default depth. */

LJ_FUNC SBufExt * LJ_FASTCALL lj_serialize_json_put(SBufExt *sbx, cTValue *o);
LJ_FUNC char * LJ_FASTCALL lj_serialize_json_get(SBufExt *sbx, TValue *o);
LJ_FUNC GCstr * LJ_FASTCALL lj_serialize_json_encode(lua_State *L, cTValue *o);
LJ_FUNC void lj_serialize_json_decode(lua_State *L, TValue *o, GCstr *str);

#endif

#endif