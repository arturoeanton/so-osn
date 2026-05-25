/*
 * plugin-static.h hand-crafted para osnos.
 *
 * lighttpd's plugin.c lo incluye dos veces (con macros distintas) para
 * declarar las init funcs y armar la load_functions table. Normalmente
 * se genera en build time desde la lista de plugins compilados.
 *
 * Subset mínimo para servir static files:
 *   mod_indexfile  — index.html lookup
 *   mod_staticfile — sirve archivos del disco
 *   mod_access     — IP allow/deny (vacío por config)
 *   mod_alias      — aliases (vacío por config)
 *   mod_setenv     — set headers (vacío por config)
 *   mod_expire     — Cache-Control (vacío por config)
 *   mod_redirect   — URL redirects (vacío por config)
 *   mod_simple_vhost / mod_evhost / mod_rewrite — vhost / rewrite
 *     (vacío por config, pero el code los requiere linkeados con
 *     LIGHTTPD_STATIC)
 */

PLUGIN_INIT(mod_rewrite)
PLUGIN_INIT(mod_redirect)
PLUGIN_INIT(mod_access)
PLUGIN_INIT(mod_alias)
PLUGIN_INIT(mod_indexfile)
PLUGIN_INIT(mod_staticfile)
PLUGIN_INIT(mod_setenv)
PLUGIN_INIT(mod_expire)
PLUGIN_INIT(mod_simple_vhost)
PLUGIN_INIT(mod_evhost)
