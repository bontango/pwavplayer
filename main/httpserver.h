//----------------------------------------------------------------------------------------
//
// httpserver.h -- HTTP REST config server for pwavplayer
//
// Mirror of the LISYclock HTTP API.  Port 8080.  Same endpoints so the same
// web editor can talk to both devices.
//
// Endpoints:
//   GET    /status           -> {"status":"ok","version":"...","api_version":1}
//   GET    /config           -> /sdcard/config.txt (text/plain)
//   POST   /config           -> write body to /sdcard/config.txt
//   GET    /files            -> JSON {"files":[{name,size,mtime},...]}
//   GET    /files/<name>     -> binary download
//   PUT    /files/<name>     -> upload body to /sdcard/<name>
//   DELETE /files/<name>     -> delete
//   POST   /rename           -> JSON {"old_name":"..","new_name":".."}
//   POST   /time             -> JSON {"unix_timestamp":N}
//   POST   /reboot           -> JSON {"confirm":"reboot"}
//   POST   /update           -> body -> /sdcard/update.bin, reboot
//   POST   /play             -> JSON {"id":N} — trigger sound
//   GET    /activity[?since=N] -> JSON {"next":N,"entries":[{seq,ts,msg},...]}
//   OPTIONS *                -> CORS preflight, 204
//
#pragma once

#include "esp_err.h"

#define HTTP_API_VERSION        1
#define HTTP_API_VERSION_STR    "1"
#define HTTP_SERVER_PORT        8080

esp_err_t httpserver_start(void);
