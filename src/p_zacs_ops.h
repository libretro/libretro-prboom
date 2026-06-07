/* generated: regular variable/array pcode cases */
    case PCD_ASSIGNSCRIPTVAR:
      { int vi = NEXTBYTE; ZVAR_SET(script, vi, ZPOP()); }
      break;
    case PCD_ADDSCRIPTVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(script, vi, ZVAR_GET(script, vi) + v); }
      break;
    case PCD_SUBSCRIPTVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(script, vi, ZVAR_GET(script, vi) - v); }
      break;
    case PCD_MULSCRIPTVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(script, vi, ZVAR_GET(script, vi) * v); }
      break;
    case PCD_ANDSCRIPTVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(script, vi, ZVAR_GET(script, vi) & v); }
      break;
    case PCD_ORSCRIPTVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(script, vi, ZVAR_GET(script, vi) | v); }
      break;
    case PCD_EORSCRIPTVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(script, vi, ZVAR_GET(script, vi) ^ v); }
      break;
    case PCD_LSSCRIPTVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(script, vi, ZVAR_GET(script, vi) << v); }
      break;
    case PCD_RSSCRIPTVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(script, vi, ZVAR_GET(script, vi) >> v); }
      break;
    case PCD_DIVSCRIPTVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(script, vi, v ? ZVAR_GET(script, vi) / v : 0); }
      break;
    case PCD_MODSCRIPTVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(script, vi, v ? ZVAR_GET(script, vi) % v : 0); }
      break;
    case PCD_PUSHSCRIPTVAR:
      { int vi = NEXTBYTE; ZPUSH(ZVAR_GET(script, vi)); }
      break;
    case PCD_INCSCRIPTVAR:
      { int vi = NEXTBYTE; ZVAR_SET(script, vi, ZVAR_GET(script, vi) + 1); }
      break;
    case PCD_DECSCRIPTVAR:
      { int vi = NEXTBYTE; ZVAR_SET(script, vi, ZVAR_GET(script, vi) - 1); }
      break;
    case PCD_ASSIGNSCRIPTARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(script, ai, ix, val); }
      break;
    case PCD_ADDSCRIPTARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(script, ai, ix, ZARR_GET(script, ai, ix) + val); }
      break;
    case PCD_SUBSCRIPTARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(script, ai, ix, ZARR_GET(script, ai, ix) - val); }
      break;
    case PCD_MULSCRIPTARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(script, ai, ix, ZARR_GET(script, ai, ix) * val); }
      break;
    case PCD_ANDSCRIPTARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(script, ai, ix, ZARR_GET(script, ai, ix) & val); }
      break;
    case PCD_ORSCRIPTARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(script, ai, ix, ZARR_GET(script, ai, ix) | val); }
      break;
    case PCD_EORSCRIPTARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(script, ai, ix, ZARR_GET(script, ai, ix) ^ val); }
      break;
    case PCD_LSSCRIPTARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(script, ai, ix, ZARR_GET(script, ai, ix) << val); }
      break;
    case PCD_RSSCRIPTARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(script, ai, ix, ZARR_GET(script, ai, ix) >> val); }
      break;
    case PCD_DIVSCRIPTARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(script, ai, ix, val ? ZARR_GET(script, ai, ix) / val : 0); }
      break;
    case PCD_MODSCRIPTARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(script, ai, ix, val ? ZARR_GET(script, ai, ix) % val : 0); }
      break;
    case PCD_PUSHSCRIPTARRAY:
      { int ai = NEXTBYTE; int ix = ZPOP(); ZPUSH(ZARR_GET(script, ai, ix)); }
      break;
    case PCD_INCSCRIPTARRAY:
      { int ai = NEXTBYTE; int ix = ZPOP();
        ZARR_SET(script, ai, ix, ZARR_GET(script, ai, ix) + 1); }
      break;
    case PCD_DECSCRIPTARRAY:
      { int ai = NEXTBYTE; int ix = ZPOP();
        ZARR_SET(script, ai, ix, ZARR_GET(script, ai, ix) - 1); }
      break;
    case PCD_ASSIGNMAPVAR:
      { int vi = NEXTBYTE; ZVAR_SET(map, vi, ZPOP()); }
      break;
    case PCD_ADDMAPVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(map, vi, ZVAR_GET(map, vi) + v); }
      break;
    case PCD_SUBMAPVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(map, vi, ZVAR_GET(map, vi) - v); }
      break;
    case PCD_MULMAPVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(map, vi, ZVAR_GET(map, vi) * v); }
      break;
    case PCD_ANDMAPVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(map, vi, ZVAR_GET(map, vi) & v); }
      break;
    case PCD_ORMAPVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(map, vi, ZVAR_GET(map, vi) | v); }
      break;
    case PCD_EORMAPVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(map, vi, ZVAR_GET(map, vi) ^ v); }
      break;
    case PCD_LSMAPVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(map, vi, ZVAR_GET(map, vi) << v); }
      break;
    case PCD_RSMAPVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(map, vi, ZVAR_GET(map, vi) >> v); }
      break;
    case PCD_DIVMAPVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(map, vi, v ? ZVAR_GET(map, vi) / v : 0); }
      break;
    case PCD_MODMAPVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(map, vi, v ? ZVAR_GET(map, vi) % v : 0); }
      break;
    case PCD_PUSHMAPVAR:
      { int vi = NEXTBYTE; ZPUSH(ZVAR_GET(map, vi)); }
      break;
    case PCD_INCMAPVAR:
      { int vi = NEXTBYTE; ZVAR_SET(map, vi, ZVAR_GET(map, vi) + 1); }
      break;
    case PCD_DECMAPVAR:
      { int vi = NEXTBYTE; ZVAR_SET(map, vi, ZVAR_GET(map, vi) - 1); }
      break;
    case PCD_ASSIGNMAPARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(map, ai, ix, val); }
      break;
    case PCD_ADDMAPARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(map, ai, ix, ZARR_GET(map, ai, ix) + val); }
      break;
    case PCD_SUBMAPARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(map, ai, ix, ZARR_GET(map, ai, ix) - val); }
      break;
    case PCD_MULMAPARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(map, ai, ix, ZARR_GET(map, ai, ix) * val); }
      break;
    case PCD_ANDMAPARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(map, ai, ix, ZARR_GET(map, ai, ix) & val); }
      break;
    case PCD_ORMAPARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(map, ai, ix, ZARR_GET(map, ai, ix) | val); }
      break;
    case PCD_EORMAPARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(map, ai, ix, ZARR_GET(map, ai, ix) ^ val); }
      break;
    case PCD_LSMAPARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(map, ai, ix, ZARR_GET(map, ai, ix) << val); }
      break;
    case PCD_RSMAPARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(map, ai, ix, ZARR_GET(map, ai, ix) >> val); }
      break;
    case PCD_DIVMAPARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(map, ai, ix, val ? ZARR_GET(map, ai, ix) / val : 0); }
      break;
    case PCD_MODMAPARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(map, ai, ix, val ? ZARR_GET(map, ai, ix) % val : 0); }
      break;
    case PCD_PUSHMAPARRAY:
      { int ai = NEXTBYTE; int ix = ZPOP(); ZPUSH(ZARR_GET(map, ai, ix)); }
      break;
    case PCD_INCMAPARRAY:
      { int ai = NEXTBYTE; int ix = ZPOP();
        ZARR_SET(map, ai, ix, ZARR_GET(map, ai, ix) + 1); }
      break;
    case PCD_DECMAPARRAY:
      { int ai = NEXTBYTE; int ix = ZPOP();
        ZARR_SET(map, ai, ix, ZARR_GET(map, ai, ix) - 1); }
      break;
    case PCD_ASSIGNWORLDVAR:
      { int vi = NEXTBYTE; ZVAR_SET(world, vi, ZPOP()); }
      break;
    case PCD_ADDWORLDVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(world, vi, ZVAR_GET(world, vi) + v); }
      break;
    case PCD_SUBWORLDVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(world, vi, ZVAR_GET(world, vi) - v); }
      break;
    case PCD_MULWORLDVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(world, vi, ZVAR_GET(world, vi) * v); }
      break;
    case PCD_ANDWORLDVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(world, vi, ZVAR_GET(world, vi) & v); }
      break;
    case PCD_ORWORLDVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(world, vi, ZVAR_GET(world, vi) | v); }
      break;
    case PCD_EORWORLDVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(world, vi, ZVAR_GET(world, vi) ^ v); }
      break;
    case PCD_LSWORLDVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(world, vi, ZVAR_GET(world, vi) << v); }
      break;
    case PCD_RSWORLDVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(world, vi, ZVAR_GET(world, vi) >> v); }
      break;
    case PCD_DIVWORLDVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(world, vi, v ? ZVAR_GET(world, vi) / v : 0); }
      break;
    case PCD_MODWORLDVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(world, vi, v ? ZVAR_GET(world, vi) % v : 0); }
      break;
    case PCD_PUSHWORLDVAR:
      { int vi = NEXTBYTE; ZPUSH(ZVAR_GET(world, vi)); }
      break;
    case PCD_INCWORLDVAR:
      { int vi = NEXTBYTE; ZVAR_SET(world, vi, ZVAR_GET(world, vi) + 1); }
      break;
    case PCD_DECWORLDVAR:
      { int vi = NEXTBYTE; ZVAR_SET(world, vi, ZVAR_GET(world, vi) - 1); }
      break;
    case PCD_ASSIGNWORLDARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(world, ai, ix, val); }
      break;
    case PCD_ADDWORLDARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(world, ai, ix, ZARR_GET(world, ai, ix) + val); }
      break;
    case PCD_SUBWORLDARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(world, ai, ix, ZARR_GET(world, ai, ix) - val); }
      break;
    case PCD_MULWORLDARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(world, ai, ix, ZARR_GET(world, ai, ix) * val); }
      break;
    case PCD_ANDWORLDARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(world, ai, ix, ZARR_GET(world, ai, ix) & val); }
      break;
    case PCD_ORWORLDARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(world, ai, ix, ZARR_GET(world, ai, ix) | val); }
      break;
    case PCD_EORWORLDARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(world, ai, ix, ZARR_GET(world, ai, ix) ^ val); }
      break;
    case PCD_LSWORLDARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(world, ai, ix, ZARR_GET(world, ai, ix) << val); }
      break;
    case PCD_RSWORLDARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(world, ai, ix, ZARR_GET(world, ai, ix) >> val); }
      break;
    case PCD_DIVWORLDARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(world, ai, ix, val ? ZARR_GET(world, ai, ix) / val : 0); }
      break;
    case PCD_MODWORLDARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(world, ai, ix, val ? ZARR_GET(world, ai, ix) % val : 0); }
      break;
    case PCD_PUSHWORLDARRAY:
      { int ai = NEXTBYTE; int ix = ZPOP(); ZPUSH(ZARR_GET(world, ai, ix)); }
      break;
    case PCD_INCWORLDARRAY:
      { int ai = NEXTBYTE; int ix = ZPOP();
        ZARR_SET(world, ai, ix, ZARR_GET(world, ai, ix) + 1); }
      break;
    case PCD_DECWORLDARRAY:
      { int ai = NEXTBYTE; int ix = ZPOP();
        ZARR_SET(world, ai, ix, ZARR_GET(world, ai, ix) - 1); }
      break;
    case PCD_ASSIGNGLOBALVAR:
      { int vi = NEXTBYTE; ZVAR_SET(global, vi, ZPOP()); }
      break;
    case PCD_ADDGLOBALVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(global, vi, ZVAR_GET(global, vi) + v); }
      break;
    case PCD_SUBGLOBALVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(global, vi, ZVAR_GET(global, vi) - v); }
      break;
    case PCD_MULGLOBALVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(global, vi, ZVAR_GET(global, vi) * v); }
      break;
    case PCD_ANDGLOBALVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(global, vi, ZVAR_GET(global, vi) & v); }
      break;
    case PCD_ORGLOBALVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(global, vi, ZVAR_GET(global, vi) | v); }
      break;
    case PCD_EORGLOBALVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(global, vi, ZVAR_GET(global, vi) ^ v); }
      break;
    case PCD_LSGLOBALVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(global, vi, ZVAR_GET(global, vi) << v); }
      break;
    case PCD_RSGLOBALVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(global, vi, ZVAR_GET(global, vi) >> v); }
      break;
    case PCD_DIVGLOBALVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(global, vi, v ? ZVAR_GET(global, vi) / v : 0); }
      break;
    case PCD_MODGLOBALVAR:
      { int vi = NEXTBYTE; int v = ZPOP();
        ZVAR_SET(global, vi, v ? ZVAR_GET(global, vi) % v : 0); }
      break;
    case PCD_PUSHGLOBALVAR:
      { int vi = NEXTBYTE; ZPUSH(ZVAR_GET(global, vi)); }
      break;
    case PCD_INCGLOBALVAR:
      { int vi = NEXTBYTE; ZVAR_SET(global, vi, ZVAR_GET(global, vi) + 1); }
      break;
    case PCD_DECGLOBALVAR:
      { int vi = NEXTBYTE; ZVAR_SET(global, vi, ZVAR_GET(global, vi) - 1); }
      break;
    case PCD_ASSIGNGLOBALARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(global, ai, ix, val); }
      break;
    case PCD_ADDGLOBALARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(global, ai, ix, ZARR_GET(global, ai, ix) + val); }
      break;
    case PCD_SUBGLOBALARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(global, ai, ix, ZARR_GET(global, ai, ix) - val); }
      break;
    case PCD_MULGLOBALARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(global, ai, ix, ZARR_GET(global, ai, ix) * val); }
      break;
    case PCD_ANDGLOBALARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(global, ai, ix, ZARR_GET(global, ai, ix) & val); }
      break;
    case PCD_ORGLOBALARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(global, ai, ix, ZARR_GET(global, ai, ix) | val); }
      break;
    case PCD_EORGLOBALARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(global, ai, ix, ZARR_GET(global, ai, ix) ^ val); }
      break;
    case PCD_LSGLOBALARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(global, ai, ix, ZARR_GET(global, ai, ix) << val); }
      break;
    case PCD_RSGLOBALARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(global, ai, ix, ZARR_GET(global, ai, ix) >> val); }
      break;
    case PCD_DIVGLOBALARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(global, ai, ix, val ? ZARR_GET(global, ai, ix) / val : 0); }
      break;
    case PCD_MODGLOBALARRAY:
      { int ai = NEXTBYTE; int val = ZPOP(); int ix = ZPOP();
        ZARR_SET(global, ai, ix, val ? ZARR_GET(global, ai, ix) % val : 0); }
      break;
    case PCD_PUSHGLOBALARRAY:
      { int ai = NEXTBYTE; int ix = ZPOP(); ZPUSH(ZARR_GET(global, ai, ix)); }
      break;
    case PCD_INCGLOBALARRAY:
      { int ai = NEXTBYTE; int ix = ZPOP();
        ZARR_SET(global, ai, ix, ZARR_GET(global, ai, ix) + 1); }
      break;
    case PCD_DECGLOBALARRAY:
      { int ai = NEXTBYTE; int ix = ZPOP();
        ZARR_SET(global, ai, ix, ZARR_GET(global, ai, ix) - 1); }
      break;
