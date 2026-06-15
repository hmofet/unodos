#!/bin/sh
cd "$(dirname "$0")/.."
echo "=== MODULES: uno_app_main export per module (want 1 each) ==="
for so in apps_store/app*.so; do
  n=$(nm -D "$so" | grep -c "uno_app_main")
  echo "  $so  uno_app_main=$n"
done
echo
echo "=== CORE OBJECT: dispatcher / game-music tick symbols ==="
nm build/unodos.o | grep -E "gm_tick|tick_all_apps|app_tick_dispatch|draw_app_content|app_key|app_close|app_opened|app_click" || echo "  (some inlined)"
